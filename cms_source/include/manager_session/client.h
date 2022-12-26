/*
 * \brief  Client-side interface of the Manager service
 * \author Kevin Burton
 * \date   2021-10-07
 */

#ifndef _INCLUDE__MANAGER_SESSION_H__CLIENT_H_
#define _INCLUDE__MANAGER_SESSION_H__CLIENT_H_

#include <manager_session/manager_session.h>
#include <base/rpc_client.h>
#include <base/log.h>

namespace Manager { struct Session_client; }


struct Manager::Session_client : Genode::Rpc_client<Session> {
	Session_client(Genode::Capability<Session> cap) : Genode::Rpc_client<Session>(cap) { }

	Genode::Ram_dataspace_capability share_mem(Genode::size_t size, Genode::uint32_t ip, Genode::uint64_t mac) override {
	    return call<Rpc_share>(size, ip, mac);
	}

	void notify(Genode::uint64_t mac, Genode::uint32_t offset, Genode::uint16_t size) override {
	    call<Rpc_notify>(mac, offset, size);
	}

	void migrate(Genode::uint64_t mac, Genode::uint32_t offset) override {
	    call<Rpc_migrate>(mac, offset);
	}
};

#endif /* _INCLUDE__MANAGER_SESSION_H__CLIENT_H_ */
