/*
 * \brief  Checkpoint manager extending the RTCR functionality
 * \author Kevin Burton
 * \date   2021-09-22
 */

/*
 * base includes
 */
#include <base/log.h>
#include <base/heap.h>
#include <base/attached_rom_dataspace.h>
#include <base/rpc_server.h>
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
 * local includes
 */
#include <manager.h>
#include <manager_session/manager_session.h>

/*
 * std includes
 */
#include <std/string>
#include <math.h>


namespace Manager {
    struct Session_component;
    struct Root_component;
    struct Broker_thread;
    struct NAS_thread;
    struct Main;
}

struct Manager::NAS_thread {
    static void *store_to_nas(void *args) {
        auto mac = ((Genode::uint64_t *) args)[0];
        auto nas_ip = ((Genode::uint32_t *) args)[2];
        auto offset = ((Genode::uint32_t *) args)[3];
        Mac_map map = *reinterpret_cast<Mac_map *>(((Genode::addr_t *) args)[4]);
        auto size = ((Genode::uint16_t *) args)[10];
        auto nas_port = ((Genode::uint16_t *) args)[11];
        Genode::addr_t mem_start = map.addr_at(mac);

        Genode::uint8_t buf[1024]; //Size of buffer subject to change
        ((Genode::uint64_t *) buf)[0] = mac;
        ((Genode::uint32_t *) buf)[2] = offset;
        buf[12] = 0; //opcode
        for (int i = 0; i < size; i++) {
            buf[i + 13] = *(reinterpret_cast<char *>(mem_start) + (offset + i) * sizeof(Genode::uint8_t));
        }
        struct sockaddr_in sockaddr = {};
        int fd;
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[NAS thread -> store] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(nas_port);
        sockaddr.sin_addr.s_addr = nas_ip;

        if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
            Genode::error("[NAS thread -> store] Socket connection failed");
            return nullptr;
        }

        if (send(fd, buf, 1024, 0) != 1024) {
            Genode::error("[NAS thread -> store] Message could not be sent");
            shutdown(fd, 0);
            return nullptr;
        }

        Genode::log("[NAS thread] Checkpoint successfully sent to NAS");

        return nullptr;
    };

    static void *migrate(void *args) {
        auto mac = ((Genode::uint64_t *) args)[0];
        auto nas_ip = ((Genode::uint32_t *) args)[2];
        auto offset = ((Genode::uint32_t *) args)[3];
        Mac_map map = *reinterpret_cast<Mac_map *>(((Genode::addr_t *) args)[4]);
        auto nas_port = ((Genode::uint16_t *) args)[10];
        auto rtcr_info_port = ((Genode::uint16_t *) args)[11];
        auto rtcr_migr_port = ((Genode::uint16_t *) args)[12];

        struct sockaddr_in sockaddr = {};
        int fd;
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[NAS thread -> migrate] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(rtcr_info_port);

        Long_tuple best(0, 0);
        for (int i = 0; i < map.size(); i++) {
            Genode::uint64_t current_mac = map.mac_at_index(i);
            if (mac == current_mac) continue;
            else {
                sockaddr.sin_addr.s_addr = htonl(map.ip_at(mac));

                if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
                    Genode::error("[NAS thread -> migrate] Info socket connection failed");
                    return nullptr;
                }

                Genode::uint64_t info_buf[2];
                recv(fd, info_buf, 2 * 8, 0);

                /*
                 * The metric determining the suitability of an ECU is simply a multiplication of available caps and RAM
                 */
                Genode::uint64_t metric = info_buf[0] * info_buf[1];
                if (metric >= best.get_second()) best.set(current_mac, metric);
            }
        }

        if (best.get_first() == 0) Genode::error("Migration not possible, only one RTCR in use");
        else {
            if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                Genode::error("Socket initialization failed");
                return nullptr;
            }

            sockaddr.sin_port = htons(nas_port);
            sockaddr.sin_addr.s_addr = nas_ip;

            if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
                Genode::error("[NAS thread -> migrate] NAS socket connection failed");
                return nullptr;
            }

            Genode::uint8_t nas_buf[13];
            ((Genode::uint64_t *) nas_buf)[0] = mac;
            ((Genode::uint32_t *) nas_buf)[2] = offset;
            nas_buf[12] = 1; //opcode

            if (send(fd, nas_buf, 13, 0) != 13) {
                Genode::error("[NAS thread -> migrate] NAS message could not be sent");
                shutdown(fd, 0);
                return nullptr;
            }

            Genode::uint8_t snapshot[1024]; //Size of buffer subject to change
            recv(fd, snapshot, 1024, 0);

            if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                Genode::error("Socket initialization failed");
                return nullptr;
            }

            sockaddr.sin_port = htons(rtcr_migr_port);
            sockaddr.sin_addr.s_addr = htonl(map.ip_at(best.get_first()));

            if (connect(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
                Genode::error("[NAS thread -> migrate] Migr socket connection failed");
                return nullptr;
            }

            if (send(fd, snapshot, 1024, 0) != 1024) {
                Genode::error("[NAS thread -> migrate] Migr message could not be sent");
                shutdown(fd, 0);
                return nullptr;
            }
        }

        return nullptr;
    }
};

struct Manager::Session_component : Genode::Rpc_object<Session> {
private:
    Libc::Env &_env;

    Mac_map &_map;

    Genode::Attached_rom_dataspace rom_config{_env, "config"};
    Genode::Xml_node config{rom_config.xml()};

    Genode::String<16> _nas_ip = "";
    Genode::uint16_t _nas_port = 0;
public:
    Session_component(Libc::Env &env, Mac_map &map) : _env(env), _map(map) {
        Genode::String<16> nas_ip(config.attribute_value("nas_ip", (Genode::String<16>) ""));
        Genode::uint16_t nas_port(config.attribute_value("nas_port", (Genode::uint16_t) 0));
        _nas_ip = nas_ip;
        _nas_port = nas_port;
    }

    Genode::Ram_dataspace_capability
    share_mem(Genode::size_t size, Genode::uint32_t ip, Genode::uint64_t mac) override {
        Genode::Ram_dataspace_capability capability = _env.ram().alloc(size);
        Genode::addr_t mem_start = _env.rm().attach(capability);
        _map.insert(mac, ip, mem_start);
        return capability;
    }

    void notify(Genode::uint64_t mac, Genode::uint32_t offset, Genode::uint16_t size) override {
        Libc::with_libc([&]() {
            Genode::uint8_t store_args[24];
            ((Genode::uint64_t *) store_args)[0] = mac;
            ((Genode::uint32_t *) store_args)[2] = inet_addr(_nas_ip.string());
            ((Genode::uint32_t *) store_args)[3] = offset;
            ((Genode::addr_t *) store_args)[4] = reinterpret_cast<Genode::addr_t>(&_map);
            ((Genode::uint16_t *) store_args)[10] = size;
            ((Genode::uint16_t *) store_args)[11] = _nas_port;
            pthread_t pthread;
            pthread_create(&pthread, NULL, Manager::NAS_thread::store_to_nas, store_args);
        });
    }

    void migrate(Genode::uint64_t mac, Genode::uint32_t offset) override {
        Genode::uint16_t rtcr_info_port(config.attribute_value("rtcr_info_port", (Genode::uint16_t) 0));
        Genode::uint16_t rtcr_migr_port(config.attribute_value("rtcr_migr_port", (Genode::uint16_t) 0));
        Libc::with_libc([&]() {
            Genode::uint8_t migr_args[26];
            ((Genode::uint64_t *) migr_args)[0] = mac;
            ((Genode::uint32_t *) migr_args)[2] = inet_addr(_nas_ip.string());
            ((Genode::uint32_t *) migr_args)[3] = offset;
            ((Genode::addr_t *) migr_args)[4] = reinterpret_cast<Genode::addr_t>(&_map);
            ((Genode::uint16_t *) migr_args)[10] = _nas_port;
            ((Genode::uint16_t *) migr_args)[11] = rtcr_info_port;
            ((Genode::uint16_t *) migr_args)[12] = rtcr_migr_port;
            pthread_t pthread;
            pthread_create(&pthread, NULL, Manager::NAS_thread::migrate, migr_args);
        });
    }
};


class Manager::Root_component : public Genode::Root_component<Session_component> {

private:
    Mac_map &_map;

    Libc::Env &_env;

protected:
    Session_component *_create_session(const char *) override {
        Genode::log("Creating Manager session");
        return new(md_alloc()) Session_component(_env, _map);
    }

public:
    Root_component(Genode::Entrypoint &ep, Genode::Allocator &alloc, Libc::Env &env, Mac_map &map)
            : Genode::Root_component<Session_component>(ep, alloc), _map(map), _env(env) {
        Genode::log("Creating root component");
    }
};


struct Manager::Broker_thread {
    static void *establish_dsm(void *args) {
        auto mem_start = ((Genode::addr_t *) args)[0];
        auto mem_size = ((Genode::size_t *) args)[1];
        auto dsm_port = ((Genode::uint16_t *) args)[4];

        struct sockaddr_in sockaddr = {};
        int fd;
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            Genode::error("[broker] Socket initialization failed");
            return nullptr;
        }
        sockaddr.sin_family = AF_INET;
        sockaddr.sin_port = htons(dsm_port);
        sockaddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
            Genode::error("[broker] Socket binding failed");
            return nullptr;
        }
        if (listen(fd, 5) < 0) {
            Genode::error("[broker] Socket listen failed");
            return nullptr;
        }

        while (true) {
            struct sockaddr addr;
            socklen_t len = sizeof(addr);
            int client = accept(fd, &addr, &len);
            if (client < 0) {
                Genode::error("[broker] Socket accept failed");
                return nullptr;
            }

            Genode::log("[broker] Connection to broker successful. Establishing DSM on ", dsm_port);

            /*
             * In the final version this part would be the DSM, but for now we use a simple publish/subscribe mechanism where
             * the RTCR sends a checkpoint and tells the thread the offset from start where it's stored on its own machine and size.
             * We trust the RTCR that no other checkpoint is overwritten.
             */

            Genode::uint8_t buf[mem_size];
            recv(client, buf, mem_size, 0);
            //32b offset, 16b size, rest is checkpoint
            Genode::uint32_t offset = buf[0] * pow(2, 24) + buf[1] * pow(2, 16) + buf[2] * pow(2, 8) + buf[3];
            Genode::uint16_t size = buf[4] * pow(2, 8) + buf[5];
            for (int i = 0; i < size; i++)
                *(reinterpret_cast<char *>(mem_start) + (offset + i) * sizeof(Genode::uint8_t)) = buf[6 + i];

            Genode::log("[broker] Done updating memory");
        }
    }
};

struct Manager::Main {
    Libc::Env &env;

    /*
     * A map mapping MAC addresses to both a memory address and IP address so that checkpoints can be retrieved from the
     * dataspace given the MAC address of the notifier and messages for determining a host to migrate to can be sent sequentially.
     */
    Mac_map map{};

    /*
     * The manager has to know which ports are already in use for dsm purposes to assign new, unused ones on a new connection.
     * This is done by saving the currently highest used port + 1 to directly assign. The ephemeral port range of
     * 1025 - 65535 since the main manager interface is on 1024. Furthermore the high number of ports means that wraparounds are a non-issue.
     */
    Genode::uint16_t highest_port = 1025;

    /*
     * A sliced heap is used for allocating session objects - thereby we
     * can release objects separately.
     */
    Genode::Sliced_heap sliced_heap{env.ram(), env.rm()};

    Manager::Root_component root{env.ep(), sliced_heap, env, map};

    Genode::Attached_rom_dataspace rom_config{env, "config"};
    Genode::Xml_node config{rom_config.xml()};

    Main(Libc::Env &env) : env(env) {
        /*
         * Create an RPC object capability for the root interface and
         * announce the service to our parent.
         */
        env.parent().announce(env.ep().manage(root));

        Genode::uint16_t port(config.attribute_value("port", (Genode::uint16_t) 0));
        Genode::String<16> nas_ip(config.attribute_value("nas_ip", (Genode::String<16>) ""));
        Genode::uint16_t nas_port(config.attribute_value("nas_port", (Genode::uint16_t) 0));

        Libc::with_libc([&]() {
            struct sockaddr_in sockaddr = {};
            int fd;
            if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                Genode::error("Socket initialization failed");
                return;
            }
            sockaddr.sin_family = AF_INET;
            sockaddr.sin_port = htons(port);
            sockaddr.sin_addr.s_addr = INADDR_ANY;
            if (bind(fd, (struct sockaddr *) &sockaddr, sizeof sockaddr) < 0) {
                Genode::error("Socket binding failed");
                return;
            }
            if (listen(fd, 5) < 0) {
                Genode::error("Socket listen failed");
                return;
            }

            while (true) {
                struct sockaddr addr;
                socklen_t len = sizeof(addr);
                int client = accept(fd, &addr, &len);
                if (client < 0) {
                    Genode::error("Socket accept failed");
                    return;
                }

                Genode::uint8_t buf[19];
                recv(client, buf, 19, 0);

                auto mac = ((Genode::uint64_t *) buf)[0];

                /*
                 * Opcode 0 = establish shared memory
                 * Opcode 1 = notification of new checkpoint in memory
                 * Opcode 2 = migration/restoration
                 */
                if (buf[18] == 0) {
                    Genode::uint16_t dsm_port = highest_port++;

                    auto ip = ((Genode::uint32_t *) buf)[2];
                    auto size = ((Genode::size_t *) buf)[3];

                    Genode::Ram_dataspace_capability capability = env.ram().alloc(size);
                    Genode::addr_t mem_start = env.rm().attach(capability);

                    Genode::uint8_t broker_args[10];
                    ((Genode::addr_t *) broker_args)[0] = mem_start;
                    ((Genode::size_t *) broker_args)[1] = size;
                    ((Genode::uint16_t *) broker_args)[4] = dsm_port;
                    pthread_t pthread;
                    pthread_create(&pthread, NULL, Manager::Broker_thread::establish_dsm, broker_args);
                    map.insert(mac, ip, mem_start);

                    /*
                     * Answering the RTCR with the port it should use to establish a DSM
                     */
                    if (send(client, &dsm_port, 2, 0) != 2) {
                        Genode::error("Answer to DSM request could not be sent");
                        shutdown(fd, 0);
                        return;
                    }

                } else if (buf[18] == 1) {
                    auto offset = ((Genode::uint32_t *) buf)[3];
                    auto size = ((Genode::uint16_t *) buf)[8];

                    Genode::uint8_t store_args[24];
                    ((Genode::uint64_t *) store_args)[0] = mac;
                    ((Genode::uint32_t *) store_args)[2] = inet_addr(nas_ip.string());
                    ((Genode::uint32_t *) store_args)[3] = offset;
                    ((Genode::addr_t *) store_args)[4] = reinterpret_cast<Genode::addr_t>(&map);
                    ((Genode::uint16_t *) store_args)[10] = size;
                    ((Genode::uint16_t *) store_args)[11] = nas_port;
                    pthread_t pthread;
                    pthread_create(&pthread, NULL, Manager::NAS_thread::store_to_nas, store_args);

                } else if (buf[18] == 2) {
                    auto offset = ((Genode::uint32_t *) buf)[3];
                    Genode::uint16_t rtcr_info_port(config.attribute_value("rtcr_info_port", (Genode::uint16_t) 0));
                    Genode::uint16_t rtcr_migr_port(config.attribute_value("rtcr_migr_port", (Genode::uint16_t) 0));

                    Genode::uint8_t migr_args[26];
                    ((Genode::uint64_t *) migr_args)[0] = mac;
                    ((Genode::uint32_t *) migr_args)[2] = inet_addr(nas_ip.string());
                    ((Genode::uint32_t *) migr_args)[3] = offset;
                    ((Genode::addr_t *) migr_args)[4] = reinterpret_cast<Genode::addr_t>(&map);
                    ((Genode::uint16_t *) migr_args)[10] = nas_port;
                    ((Genode::uint16_t *) migr_args)[11] = rtcr_info_port;
                    ((Genode::uint16_t *) migr_args)[12] = rtcr_migr_port;
                    pthread_t pthread;
                    pthread_create(&pthread, NULL, Manager::NAS_thread::migrate, migr_args);
                } else {
                    throw Manager::Message_format_exception();
                }
            }
        });
    }
};

void Libc::Component::construct(Libc::Env &env) {
    static Manager::Main main(env);
}
