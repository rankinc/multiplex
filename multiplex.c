#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <poll.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/types.h>
#include <syslog.h>
#include <stdbool.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#ifndef SA_RESTART
#  define SA_RESTART  0
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

/*
 * Copied from evtest.c
 */
#define BITS_PER_LONG         (sizeof(long) * 8)
#define NBITS(x)              ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)                ((x)%BITS_PER_LONG)
#define LONG(x)               ((x)/BITS_PER_LONG)
#define test_bit(bit, array)  ((array[LONG(bit)] >> OFF(bit)) & 1)

struct multiplex {
    struct libevdev *dev;
    int inputCount;
    struct libevdev **input;
    struct libevdev_uinput *output;
};

static sigjmp_buf jump;
static volatile sig_atomic_t isJumping;

static volatile sig_atomic_t isTerminated;
static const char DVB_IR[] = "dvb-ir-";


static void terminate(int signo) {
    isTerminated = true;
    (void)signo;

    if (isJumping) {
        isJumping = false;
        siglongjmp(jump, 1);
    }
}


static int openEvent(const char *devName, struct libevdev **device) {
    int err = -1;

    int fd = open(devName, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        err = libevdev_new_from_fd(fd, device);
    }

    if (err != 0) {
        syslog(LOG_ERR, "Failed to open %s: %s\n",
                   devName, strerror(errno));
    }

    return err;
}


/**
 * Copy event type configuration from input device to output device.
 */
static void copyEventType(int type, const struct libevdev *input, struct libevdev *output) {
    unsigned long bit[NBITS(KEY_CNT)] = {};
    ioctl(libevdev_get_fd(input), EVIOCGBIT(type, sizeof(bit)), bit);

    for (unsigned int code = 0; code < KEY_CNT; ++code) {
        if (test_bit(code, bit)) {
            libevdev_enable_event_code(output, type, code, NULL);
        }
    }
}


static int filter_dvb(const struct dirent* entry) {
    return strncmp(entry->d_name, DVB_IR, sizeof(DVB_IR) - 1) == 0;
}

static int openInputs(struct multiplex *multi) {
    static const char INPUT_DEVICES[] = "/dev/input/";

    struct dirent** devices;
    const int found = scandir(INPUT_DEVICES, &devices, filter_dvb, NULL);
    int result = found;
    int count = 0;

    if (found > 0) {
        multi->input = calloc(found, sizeof(multi->input[0]));
        if (multi->input == NULL) {
            syslog(LOG_ERR, "Failed to allocate input devices\n");
            result = -1;
        } else {
            char buffer[sizeof(INPUT_DEVICES) + sizeof(devices[0]->d_name)];
            struct libevdev** inputDevice = multi->input;
            count = found;

            for (int i = 0; i < found; ++i, ++inputDevice) {
                snprintf(buffer, sizeof(buffer),
                         "%s%s", INPUT_DEVICES, devices[i]->d_name);
                buffer[sizeof(buffer) - 1] = '\0';
                syslog(LOG_INFO, "Multiplexing %s\n", buffer);

                if (openEvent(buffer, inputDevice) != 0) {
                    /*
                     * FAIL! So release every resource
                     * allocated so far, then exit.
                     */
                    while (i > 0) {
                        libevdev_free(*(--inputDevice));
                        --i;
                    }
                    free(multi->input);
                    multi->input = NULL;
                    result = -1;
                    count = 0;
                    break;
                }
            } /* for */
        }

        /*
         * Release memory from directory scan.
         */
        for (int i = 0; i < found; ++i) {
            free(devices[i]);
        }
        free(devices);
    } else {
        multi->input = NULL;

        if (found == -1) {
            syslog(LOG_ERR, "Error searching for dvb-ir sub-devices: %s\n",
                            strerror(errno));
        } else {
            free(devices);
        }
    }

    multi->inputCount = count;
    return result;
}


static void closeInputs(struct multiplex *multi) {
    struct libevdev** inputDevice = multi->input;
    int i = multi->inputCount;

    while (i > 0) {
        libevdev_free(*inputDevice);
        ++inputDevice;
        --i;
    }
    free(multi->input);
    multi->input = NULL;
    multi->inputCount = 0;
}


static int openMultiplex(struct multiplex *multi) {
    static const int event_type[] = { EV_KEY, EV_MSC, EV_REP, EV_REL, EV_LED };
    struct libevdev *dev;
    int err = openInputs(multi);

    if (err <= 0) {
        syslog(LOG_ERR, "Failed to open input devices\n");
        return -1;
    }

    dev = libevdev_new();
    libevdev_set_name(dev, "multiplex");

    /*
     * Configure the multiplex device to handle the same
     * events as all of its input devices.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(event_type); ++i) {
        const int eventType = event_type[i];
        libevdev_enable_event_type(dev, eventType);

        struct libevdev** inputDevice = multi->input;
        int j = multi->inputCount;
        while (j > 0) {
            copyEventType(eventType, *inputDevice, dev);
            ++inputDevice;
            --j;
        }
    }

    err = libevdev_uinput_create_from_device(dev,
        LIBEVDEV_UINPUT_OPEN_MANAGED,
        &multi->output
    );

    if (err != 0) {
        syslog(LOG_ERR, "Failed to open output device: %s\n", strerror(errno));
        libevdev_free(dev);
        closeInputs(multi);
        return err;
    }

    multi->dev = dev;
    return 0;
}


static void closeMultiplex(struct multiplex *multi) {
    libevdev_uinput_destroy(multi->output);
    libevdev_free(multi->dev);
    closeInputs(multi);
}


static int copy(struct pollfd *fds, struct libevdev *input, struct libevdev_uinput *output) {
    int err = 0;

    if ((fds->revents & (POLLHUP | POLLERR)) != 0) {
        return -ENODEV;
    } else if ((fds->revents & POLLIN) != 0) {
        struct input_event ev;

        do {
            err = libevdev_next_event(
                input,
                LIBEVDEV_READ_FLAG_NORMAL,
                &ev
            );

            if (err == 0) {
                err = libevdev_uinput_write_event(
                    output, ev.type, ev.code, ev.value
                );
            }
        } while (err == 0);
    }

    return err;
}


static bool copyEvents(struct pollfd *fds, struct multiplex *multi) {
    struct libevdev** inputDevice = multi->input;
    int i = multi->inputCount;

    while (i > 0) {
        if (copy(fds, *inputDevice, multi->output) == -ENODEV) {
            return false;
        }
        ++inputDevice;
        ++fds;
        --i;
    }

    return true;
}


static void process(struct multiplex *multi) {
    struct pollfd* fds = calloc(multi->inputCount, sizeof(struct pollfd));
    if (fds == NULL) {
        syslog(LOG_ERR, "Failed to allocate polling memory\n");
        return;
    }

    {
        struct libevdev** inputDevice = multi->input;
        struct pollfd* iter = fds;
        int i = multi->inputCount;

        /*
         * Add the input descriptors into the array.
         */
        while (i > 0) {
            iter->events = POLLIN;
            iter->fd = libevdev_get_fd(*inputDevice);
            ++inputDevice;
            ++iter;
            --i;
        }
    }

    /*
     * poll() always returns -EINTR when a signal is received,
     * regardless of sigaction's SA_RESTART flag.
     * However, the non-local jump will abort the poll when
     * it receives SIGTERM anyway.
     */
    if (sigsetjmp(jump, 1) == 0) {
        isJumping = true;

        /*
         * The non-local jump closes a race between
         * testing the while condition and waiting
         * for the next input event.
         */
        while (!isTerminated) {
            int err = poll(fds, multi->inputCount, -1);
            isJumping = false;

            /*
             * The non-local jump has been disabled,
             * so we're now allowed to be non-reentrant.
             */
            if ((err > 0) && !copyEvents(fds, multi)) {
                break;
            }

            /*
             * Reenable the non-local jump before
             * we retest the while condition.
             */
            isJumping = true;
        }

        /*
         * Disable the non-local jump permanently.
         */
        isJumping = false;
    }

    free(fds);
}


static pid_t daemonise(const struct passwd* user) {
    pid_t pid;

    if ((pid = fork()) != 0) {
        // Parent process returns here.
        return pid;
    }

    if (fork() != 0) {
        // Child process exits here.
        _exit(EXIT_SUCCESS);
    }

    /*
     * Grandchild process drops privileges, switches to filesystem
     * root and closes any terminal it may have inherited.
     */
    if (user != NULL) {
        setgid(user->pw_gid);
        if ((setuid(user->pw_uid) != 0) && (errno != EPERM)) {
            syslog(LOG_ALERT, "Failed to drop privileges: %s", strerror(errno));
            return -1;
        }
    }

    setsid();
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return 0;
}


static void setup() {
    struct sigaction sigact;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = (SA_RESTART | SA_NODEFER);
    sigact.sa_handler = terminate;
    sigaction(SIGTERM, &sigact, NULL);

    openlog("multiplex", 0, LOG_DAEMON);
}


int main(int argc, char *const argv[]) {
    struct multiplex multi = { .dev = NULL, .inputCount = 0, .input = NULL };
    const char* username = "vdr";
    bool testMode = false;
    int err;

    while ((err = getopt(argc, argv, "tu:")) != -1) {
        switch(err) {
        case 't':
            printf("Running in test mode\n");
            testMode = true;
            break;

        case 'u':
            username = optarg;
            break;

        default:
            break;
        }
    }

    setup();

    errno = 0;
    const struct passwd* user = getpwnam(username);
    if (user == NULL) {
        if (errno != 0) {
            syslog(LOG_ERR, "User '%s': %s\n", username, strerror(errno));
        } else {
            syslog(LOG_ERR, "No such user '%s'\n", username);
        }
        return EXIT_FAILURE;
    }

    err = openMultiplex(&multi);
    if (err != 0) {
        syslog(LOG_ERR, "Failed to open multiplex device");
        return EXIT_FAILURE;
    }

    if (testMode || ((err = daemonise(user)) == 0)) {
        process(&multi);

        closeMultiplex(&multi);
    }

    return (err >= 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

