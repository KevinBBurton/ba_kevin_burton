/*
 * \brief  Component simulating a network addressed storage to store checkpoints in
 * \author Kevin Burton
 * \date   2021-10-11
 */

/*
 * base includes
 */
#include <base/log.h>
#include <base/heap.h>
#include <base/attached_rom_dataspace.h>

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
#include <math.h>

/*
 * local includes
 */
#include <manager.h>

namespace NAS {
    struct Main;
    struct Checkpoint;
}

struct NAS::Checkpoint {
    /*
     * A combination of mac and offset are used as primary key, as the mac is unique among all ECUs and the offset is unique
     * for all protected processes of the RTCR
     */
    Genode::uint64_t mac;
    Genode::uint32_t offset;
    Genode::uint8_t *snapshot;

    /*
     * The Genode <std/vector> library for some reason doesn't allow objects, therefore we have to fall back to a naive linked
     * list implementation
     */
    NAS::Checkpoint *previous;
    NAS::Checkpoint *next;

    Checkpoint(Genode::uint64_t mac, Genode::uint32_t offset, uint8_t *snapshot, NAS::Checkpoint *previous,
               NAS::Checkpoint *next) : mac(mac), offset(offset), snapshot(snapshot), previous(previous), next(next) {}

    ~Checkpoint() = default;
};

struct NAS::Main {
    Libc::Env &env;

    Genode::Sliced_heap sliced_heap{env.ram(), env.rm()};

    Genode::addr_t last;

    Main(Libc::Env &env) : env(env), last(0) {
        Genode::Attached_rom_dataspace rom_config(env, "config");
        Genode::Xml_node config(rom_config.xml());
        Genode::uint16_t port(config.attribute_value("port", (Genode::uint16_t) 0));

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

                Genode::uint8_t buf[1024]; //Size of buffer subject to change
                recv(client, buf, 1024, 0);

                auto mac = ((Genode::uint64_t *) buf)[0];
                auto offset = ((Genode::uint32_t *) buf)[2];

                /*
                 * Opcode 0 = store new checkpoint
                 * Opcode 1 = get a checkpoint for migration
                 */
                if (buf[12] == 0) {
                    auto *snapshot = new(sliced_heap) Genode::uint8_t[1024 - 13];
                    for (int i = 0; i < 1024 - 13; i++) snapshot[i] = buf[i + 13];

                    auto checkpoint = new(sliced_heap) NAS::Checkpoint(mac, offset, snapshot,
                                                                       reinterpret_cast<NAS::Checkpoint *>(last),
                                                                       nullptr);
                    if (last != 0) reinterpret_cast<NAS::Checkpoint *>(last)->next = checkpoint;
                    last = reinterpret_cast<Genode::addr_t>(checkpoint);

                    Genode::log("Checkpoint was stored successfully");

                } else if (buf[12] == 1) {
                    auto current = reinterpret_cast<NAS::Checkpoint *>(last);
                    while (true) {
                        if (current == nullptr) {
                            Genode::error("Checkpoint for this MAC and offset could not be found");
                            break;
                        } else if (current->mac == mac && current->offset == offset) {
                            Genode::log("Checkpoint retrieved, sending to manager");
                            if (send(client, current->snapshot, 1024, 0) != 1024) {
                                Genode::error("Message could not be sent");
                                shutdown(fd, 0);
                                return;
                            }

                            if (current->previous != nullptr) current->previous->next = current->next;
                            if (current->next != nullptr) current->next->previous = current->previous;
                            Genode::destroy(sliced_heap, current);
                            break;
                        }
                        current = current->previous;
                    }
                } else {
                    throw Manager::Message_format_exception();
                }

            }
        });
    }
};

void Libc::Component::construct(Libc::Env &env) {
    static NAS::Main main(env);
}