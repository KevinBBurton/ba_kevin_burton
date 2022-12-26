/*
 * \brief  Connection to Manager service
 * \author Kevin Burton
 * \date   2021-10-07
 */

#ifndef _INCLUDE__MANAGER_SESSION__CONNECTION_H_
#define _INCLUDE__MANAGER_SESSION__CONNECTION_H_

#include <manager_session/client.h>
#include <base/connection.h>

namespace Manager { struct Connection; }


struct Manager::Connection : Genode::Connection<Session>, Session_client
{
	Connection(Genode::Env &env)
	:
		/* create session */
		Genode::Connection<Manager::Session>(env, session(env.parent(),
		                                                "ram_quota=6K, cap_quota=100")),

		/* initialize RPC interface */
		Session_client(cap()) { }
};

#endif /* _INCLUDE__MANAGER_SESSION__CONNECTION_H_ */
