/*
 * \brief  RTCR simulating an RTCR component
 * \author Kevin Burton
 * \date   2021-09-22
 */

/*
 * base includes
 */
#include <base/attached_rom_dataspace.h>
#include <base/log.h>
#include <base/heap.h>
#include <base/rpc_server.h>
#include <timer_session/connection.h>
#include <root/component.h>

/*
 * libc includes
 */
#include <libc/component.h>

/*
 * socket includes
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/*
 * std includes
 */
#include <std/string>
#include <math.h>

namespace RTCR {
    struct Main;
    struct Info_thread;
    struct Migr_thread;
    struct Broker_thread;
}

struct RTCR::Info_thread {
    static void *platform_info(void *args) {
        auto env = reinterpret_cast<Libc::Env *>(((Genode::addr_t *) args)[0]);
        auto port = ((Genode::uint16_t *) args)[2];

        struct sockaddr_in sockaddr = {};
        int fd;
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[Info thread] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(port);
        sockaddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
            Genode::error("[Info thread] Socket binding failed");
            return nullptr;
        }
        if (listen(fd, 5) < 0) {
            Genode::error("[Info thread] Socket listen failed");
            return nullptr;
        }

        while (true) {
            struct sockaddr addr;
            socklen_t len = sizeof(addr);
            int client = accept(fd, &addr, &len);
            if (client < 0) {
                Genode::error("[Info thread] Socket accept failed");
                return nullptr;
            }

            /*
             * The caller gets as information both the available RAM in bytes and the available Caps
             */
            Genode::uint64_t buf[2] = {
                    env->pd().avail_ram().value,
                    env->pd().avail_caps().value
            };

            if (send(client, buf, 2 * 8, 0) != 2 * 8) {
                Genode::error("[Info thread] Message could not be sent");
                shutdown(fd, 0);
                return nullptr;
            }
        }
    }
};

struct RTCR::Migr_thread {
    static void *restore_interface(void *args) {
        Genode::uint16_t port = ((Genode::uint16_t *) args)[0];

        struct sockaddr_in sockaddr = {};
        int fd;
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[Migr thread] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(port);
        sockaddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
            Genode::error("[Info thread] Socket binding failed");
            return nullptr;
        }
        if (listen(fd, 5) < 0) {
            Genode::error("[Migr thread] Socket listen failed");
            return nullptr;
        }

        while (true) {
            struct sockaddr addr;
            socklen_t len = sizeof(addr);
            int client = accept(fd, &addr, &len);
            if (client < 0) {
                Genode::error("[Migr thread] Socket accept failed");
                return nullptr;
            }

            char snapshot[1024];
            recv(client, snapshot, 1024, 0);
            Genode::String<19> output(snapshot);
            Genode::log("[Migr thread] Checkpoint ", output, " received. Migration successful");
        }
    }
};

struct RTCR::Broker_thread {
    static void *establish_dsm(void *args) {
        auto config = reinterpret_cast<Genode::Xml_node *>(((Genode::addr_t *) args)[0]);
        auto dsm_port = ((Genode::uint16_t *) args)[2];

        Genode::uint16_t manager_port = config->attribute_value("manager_port", (Genode::uint16_t) 0);
        Genode::String<16> manager_ip = config->attribute_value("manager_ip", (Genode::String<16>) "0.0.0.0");
        Genode::String<16> ip = config->attribute_value("ip", (Genode::String<16>) "0.0.0.0");
        Genode::uint64_t mac = config->attribute_value("mac", (Genode::uint64_t) 0);
        Genode::String<8> name = config->attribute_value("name", (Genode::String<8>) "");

        struct sockaddr_in sockaddr = {};
        int fd;
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[broker] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(dsm_port);
        sockaddr.sin_addr.s_addr = inet_addr(manager_ip.string());

        if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
            Genode::error("[broker] Socket connection failed");
            return nullptr;
        }

        //32b offset, 16b size, rest is checkpoint
        char buf[1024] = { //Size of buffer subject to change
                0, 0, 0, 64,
                0, 8
        };
        char *snapshot = const_cast<char *>(name.string());
        for (Genode::size_t i = 0; i < name.length(); i++) buf[i + 6] = snapshot[i];

        if (send(fd, buf, 1024, 0) != 1024) {
            Genode::error("[broker] Message could not be sent");
            shutdown(fd, 0);
            return nullptr;
        }

        /*
         * Case 2: Notification of a  new CP
         */
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[broker] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(manager_port);
        sockaddr.sin_addr.s_addr = inet_addr(manager_ip.string());

        if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
            Genode::error("[broker] Socket connection failed");
            return nullptr;
        }

        /*
         * 8 bytes MAC
         * 4 bytes IP
         * 6 bytes payload
         * of which 4 bytes offset = 64B
         * of which 2 bytes size = 8B
         * 1 byte opcode = 1
         */

        Genode::uint8_t buf2[19];
        ((Genode::uint64_t *) buf2)[0] = mac;
        ((Genode::uint32_t *) buf2)[2] = htonl(inet_addr(ip.string()));
        ((Genode::uint32_t *) buf2)[3] = 64;
        ((Genode::uint16_t *) buf2)[8] = 8;
        ((Genode::uint8_t *) buf2)[18] = 1;

        if (send(fd, buf2, 19, 0) != 19) {
            Genode::error("[broker] Message could not be sent");
            shutdown(fd, 0);
            return nullptr;
        }
        Genode::log("[broker] Notification of new CP sent");

        return nullptr;
    }
};

struct RTCR::Main {
    Libc::Env &env;

    /*
     * A sliced heap is used for allocating session objects - thereby we
     * can release objects separately.
     */
    Genode::Sliced_heap sliced_heap{env.ram(), env.rm()};

    Timer::Connection timer{env};

    Genode::Attached_rom_dataspace rom_config{env, "config"};
    Genode::Xml_node config{rom_config.xml()};

    Main(Libc::Env &env) : env(env) {
        Libc::with_libc([&]() {
            /*
             * Establishing and starting the interface for CPU and RAM status
             */
            Genode::uint16_t info_port = config.attribute_value("info_port", (Genode::uint16_t) 0);

            Genode::uint8_t info_args[6];
            ((Genode::addr_t *) info_args)[0] = reinterpret_cast<Genode::addr_t>(&env);
            ((Genode::uint16_t *) info_args)[2] = info_port;
            pthread_t info_thread;
            pthread_create(&info_thread, NULL, RTCR::Info_thread::platform_info, info_args);

            /*
             * Establishing and starting the interface for migration/restoration over ethernet
             */
            Genode::uint16_t migr_port = config.attribute_value("migr_port", (Genode::uint16_t) 0);
            pthread_t rest_thread;
            pthread_create(&rest_thread, NULL, RTCR::Migr_thread::restore_interface, &migr_port);

            Genode::uint16_t manager_port = config.attribute_value("manager_port", (Genode::uint16_t) 0);
            Genode::String<16> manager_ip = config.attribute_value("manager_ip", (Genode::String<16>) "0.0.0.0");

            /*
             * Case 1: DSM establishment
             */
            struct sockaddr_in sockaddr = {};
            int fd;
            if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                Genode::error("Socket initialization failed");
                return;
            }

            sockaddr.sin_family = AF_INET;
            sockaddr.sin_port = htons(manager_port);
            sockaddr.sin_addr.s_addr = inet_addr(manager_ip.string());

            if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
                Genode::error("Socket connection failed");
                return;
            }

            Genode::String<16> ip = config.attribute_value("ip", (Genode::String<16>) "0.0.0.0");
            Genode::uint64_t mac = config.attribute_value("mac", (Genode::uint64_t) 0);

            /*
             * 8 bytes MAC
             * 4 bytes IP
             * 6 bytes payload of which 4 bytes size = 4096B
             * 1 byte opcode = 0
             */

            Genode::size_t mem_size = 4096;
            Genode::uint8_t buf[19];
            ((Genode::uint64_t *) buf)[0] = mac;
            ((Genode::uint32_t *) buf)[2] = htonl(inet_addr(ip.string()));;
            ((Genode::uint32_t *) buf)[3] = mem_size;
            ((Genode::uint8_t *) buf)[18] = 0;


            Genode::log("Connecting to manager for DSM establishment");

            if (send(fd, buf, 19, 0) != 19) {
                Genode::error("Message could not be sent");
                shutdown(fd, 0);
                return;
            }

            Genode::uint16_t dsm_port = 0;
            recv(fd, &dsm_port, 2, 0);

            /*
             * Simulation of making a checkpoint
             */
            Genode::uint8_t broker_args[6];
            ((Genode::addr_t *) broker_args)[0] = reinterpret_cast<Genode::addr_t>(&config);
            ((Genode::uint16_t *) broker_args)[2] = dsm_port;

            pthread_t broker_pthread;
            pthread_create(&broker_pthread, NULL, RTCR::Broker_thread::establish_dsm, broker_args);

            pthread_join(broker_pthread, NULL);

            /*
             * In a real system the RTCR wouldn't immediately call for migration, therefore we wait before doing so.
             * This also mitigates the possibility of one RTCR calling for migration when the other isn't even registered to the manager.
             */
            timer.msleep(3000);

            /*
             * Case 3: Call for migration and restoration
             */
            if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                Genode::error("Socket initialization failed");
                return;
            }

            sockaddr.sin_family = AF_INET;
            sockaddr.sin_port = htons(manager_port);
            sockaddr.sin_addr.s_addr = inet_addr(manager_ip.string());

            if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
                Genode::error("Socket connection failed");
                return;
            }

            /*
             * 8 bytes MAC
             * 4 bytes IP
             * 6 bytes payload of which 4 bytes offset = 64B
             * 1 byte opcode = 2
             */
            Genode::uint8_t buf2[19];
            ((Genode::uint64_t *) buf2)[0] = mac;
            ((Genode::uint32_t *) buf2)[2] = htonl(inet_addr(ip.string()));
            ((Genode::uint32_t *) buf2)[3] = 64;
            ((Genode::uint8_t *) buf2)[18] = 2;


            Genode::log("Calling for migration");

            if (send(fd, buf2, 19, 0) != 19) {
                Genode::error("Message could not be sent");
                shutdown(fd, 0);
                return;
            }
        });

    }
};

void Libc::Component::construct(Libc::Env &env) {
    static RTCR::Main main(env);
}
