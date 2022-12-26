/*
 * \brief  Interface definition of the Manager service
 * \author Kevin Burton
 * \date   2021-10-07
 */

#ifndef _INCLUDE__MANAGER_SESSION__MANAGER_SESSION_H_
#define _INCLUDE__MANAGER_SESSION__MANAGER_SESSION_H_

#include <session/session.h>
#include <base/rpc.h>

namespace Manager { struct Session; }


struct Manager::Session : Genode::Session
{
	static const char *service_name() { return "Manager"; }

	enum { CAP_QUOTA = 100 };

	virtual Genode::Ram_dataspace_capability share_mem(Genode::size_t size, Genode::uint32_t ip, Genode::uint64_t mac) = 0;

	virtual void notify(Genode::uint64_t mac, Genode::uint32_t offset, Genode::uint16_t size) = 0;

	virtual void migrate(Genode::uint64_t mac, Genode::uint32_t offset) = 0;
	/*******************
	 ** RPC interface **
	 *******************/

	GENODE_RPC(Rpc_share, Genode::Ram_dataspace_capability, share_mem, Genode::size_t, Genode::uint32_t, Genode::uint64_t);
	GENODE_RPC(Rpc_notify, void, notify, Genode::uint64_t, Genode::uint32_t, Genode::uint16_t);
	GENODE_RPC(Rpc_migrate, void, migrate, Genode::uint64_t, Genode::uint32_t);
    GENODE_RPC_INTERFACE(Rpc_notify, Rpc_share, Rpc_migrate);
};

#endif /* _INCLUDE__MANAGER_SESSION__MANAGER_SESSION_H_ */
