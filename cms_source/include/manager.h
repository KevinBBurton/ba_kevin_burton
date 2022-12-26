//
// Created by kevinburton on 28.09.21.
//

#include <std/vector>

#ifndef GENODE_MANAGER_H

namespace Manager {
    struct Message_format_exception;
}

struct Manager::Message_format_exception : Genode::Exception {
};

struct Long_tuple {
private:
    Genode::uint64_t _a;
    Genode::uint64_t _b;

public:
    Long_tuple(Genode::uint64_t a, Genode::uint64_t b) : _a(a), _b(b) {}

    Genode::uint64_t get_first() const { return _a; }

    Genode::uint64_t get_second() const { return _b; }

    void set(Genode::uint64_t a, Genode::uint64_t b) {
        _a = a;
        _b = b;
    }
};

struct Mac_map {
private:
    std::vector<Genode::uint64_t> _macs{};
    std::vector<Genode::uint32_t> _ips{};
    std::vector<Genode::addr_t> _addresses{};

public:
    Mac_map() = default;

    ~Mac_map() = default;

    void insert(Genode::uint64_t mac, Genode::uint32_t ip, Genode::addr_t addr) {
        _macs.push_back(mac);
        _ips.push_back(ip);
        _addresses.push_back(addr);
    }

    Genode::addr_t addr_at(Genode::uint64_t mac) {
        int size = (int) _macs.size();
        for (int i = 0; i < size; i++) {
            if (_macs[i] == mac) return _addresses.at(i);
        }
        return 0;
    }

    Genode::uint32_t ip_at(Genode::uint64_t mac) {
        int size = (int) _macs.size();
        for (int i = 0; i < size; i++) {
            if (_macs[i] == mac) return _ips.at(i);
        }
        return 0;
    }

    Genode::uint64_t mac_at_index(int i) {
        return _macs.at(i);
    }

    int size() {
        return _macs.size();
    }

    void remove(Genode::uint64_t mac) {
        int size = (int) _macs.size();
        for (int i = 0; i < size; i++) {
            if (_macs[i] == mac) {
                _macs.erase(_macs.begin() + i);
                _addresses.erase(_addresses.begin() + i);
                _ips.erase(_ips.begin() + i);
                return;
            }
        }
    }
};


#define GENODE_MANAGER_H

#endif //GENODE_MANAGER_H
