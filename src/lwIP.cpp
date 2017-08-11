/*
 * ZeroTier SDK - Network Virtualization Everywhere
 * Copyright (C) 2011-2017  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

// lwIP network stack driver 

#include <algorithm>

#include "libzt.h"
#include "SocketTap.hpp"
#include "Utilities.hpp"
#include "lwIP.hpp"

#include "netif/ethernet.h"
#include "lwip/etharp.h"

err_t tapif_init(struct netif *netif)
{
  DEBUG_INFO();
  return ERR_OK;
}

err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	DEBUG_INFO();
	struct pbuf *q;
	char buf[ZT_MAX_MTU+32];
	char *bufptr;
	int totalLength = 0;

	ZeroTier::SocketTap *tap = (ZeroTier::SocketTap*)netif->state;
	bufptr = buf;
	// Copy data from each pbuf, one at a time
	for(q = p; q != NULL; q = q->next) {
		memcpy(bufptr, q->payload, q->len);
		bufptr += q->len;
		totalLength += q->len;
	}
	// Split ethernet header and feed into handler
	struct eth_hdr *ethhdr;
	ethhdr = (struct eth_hdr *)buf;

	ZeroTier::MAC src_mac;
	ZeroTier::MAC dest_mac;
	src_mac.setTo(ethhdr->src.addr, 6);
	dest_mac.setTo(ethhdr->dest.addr, 6);

	tap->_handler(tap->_arg,NULL,tap->_nwid,src_mac,dest_mac,
		ZeroTier::Utils::ntoh((uint16_t)ethhdr->type),0,buf + sizeof(struct eth_hdr),totalLength - sizeof(struct eth_hdr));
	return ERR_OK;
}

namespace ZeroTier
{
	void lwIP::lwip_init_interface(SocketTap *tap, const InetAddress &ip)
	{
		DEBUG_INFO();
		Mutex::Lock _l(tap->_ips_m);

		if (std::find(tap->_ips.begin(),tap->_ips.end(),ip) == tap->_ips.end()) {
			tap->_ips.push_back(ip);
			std::sort(tap->_ips.begin(),tap->_ips.end());
#if defined(LIBZT_IPV4)
			if (ip.isV4()) {
				// Set IP
				static ip_addr_t ipaddr, netmask, gw;
				IP4_ADDR(&gw,127,0,0,1);
				ipaddr.addr = *((u32_t *)ip.rawIpData());
				netmask.addr = *((u32_t *)ip.netmask().rawIpData());
				netif_add(&(tap->lwipdev),&ipaddr, &netmask, &gw, NULL, tapif_init, ethernet_input);
				tap->lwipdev.state = tap;
				tap->lwipdev.output = etharp_output;
				tap->_mac.copyTo(tap->lwipdev.hwaddr, 6);
				tap->lwipdev.mtu = tap->_mtu;
				tap->lwipdev.name[0] = 'l';
				tap->lwipdev.name[1] = '4';
				tap->lwipdev.linkoutput = low_level_output;
				tap->lwipdev.hwaddr_len = 6;
				tap->lwipdev.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
				netif_set_default(&(tap->lwipdev));
				netif_set_up(&(tap->lwipdev));
				char ipbuf[64];
				DEBUG_INFO("addr=%s, netmask=%s", ip.toString(ipbuf), ip.netmask().toString(ipbuf));
			}
#endif
#if defined(LIBZT_IPV6)
			if(ip.isV6()) {
				DEBUG_INFO("local_addr=%s", ip.toString().c_str());
				static ip6_addr_t addr6;
				struct sockaddr_in6 in6;
				memcpy(in6.sin6_addr.s6_addr,ip.rawIpData(),16);
				in6_to_ip6((ip6_addr *)&addr6, &in6);
				tap->lwipdev6.mtu = tap->_mtu;
				tap->lwipdev6.name[0] = 'l';
				tap->lwipdev6.name[1] = '6';
				tap->lwipdev6.hwaddr_len = 6;
				tap->lwipdev6.linkoutput = low_level_output;
				tap->lwipdev6.ip6_autoconfig_enabled = 1;
				tap->_mac.copyTo(tap->lwipdev6.hwaddr, tap->lwipdev6.hwaddr_len);
				netif_create_ip6_linklocal_address(&(tap->lwipdev6), 1);
				netif_add(&(tap->lwipdev6), NULL, tapif_init, ethernet_input);
				netif_set_default(&(tap->lwipdev6));
				netif_set_up(&(tap->lwipdev6)); 
				netif_ip6_addr_set_state(&(tap->lwipdev6), 1, IP6_ADDR_TENTATIVE); 
				ip6_addr_copy(ip_2_ip6(tap->lwipdev6.ip6_addr[1]), addr6);
				tap->lwipdev6.output_ip6 = ethip6_output;
				tap->lwipdev6.state = tap;
				tap->lwipdev6.flags = NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
				char ipbuf[64];
				DEBUG_INFO("addr=%s, netmask=%s", ip.toString(ipbuf), ip.netmask().toString(ipbuf));
			}
#endif 
		}
	}

	void lwIP::lwip_loop(SocketTap *tap)
	{
	   DEBUG_INFO();
	   uint64_t prev_tcp_time = 0, prev_status_time = 0, prev_discovery_time = 0;
	   while(tap->_run)
	   {
			uint64_t now = OSUtils::now();
			uint64_t since_tcp = now - prev_tcp_time;
			uint64_t since_discovery = now - prev_discovery_time;
			uint64_t since_status = now - prev_status_time;
			uint64_t tcp_remaining = LWIP_TCP_TIMER_INTERVAL;
			uint64_t discovery_remaining = 5000;

#if defined(LIBZT_IPV6)
				#define DISCOVERY_INTERVAL 1000
#elif defined(LIBZT_IPV4)
				#define DISCOVERY_INTERVAL ARP_TMR_INTERVAL
#endif
			// Main TCP/ETHARP timer section
			if (since_tcp >= LWIP_TCP_TIMER_INTERVAL) {
				prev_tcp_time = now;
				tcp_tmr();
			} 
			else {
				tcp_remaining = LWIP_TCP_TIMER_INTERVAL - since_tcp;
			}
			if (since_discovery >= DISCOVERY_INTERVAL) {
				prev_discovery_time = now;
#if defined(LIBZT_IPV4)
					etharp_tmr();
#endif
#if defined(LIBZT_IPV6)
					nd6_tmr();
#endif
			} else {
				discovery_remaining = DISCOVERY_INTERVAL - since_discovery;
			}
			tap->_phy.poll((unsigned long)std::min(tcp_remaining,discovery_remaining));
		}
	}

	void lwIP::lwip_rx(SocketTap *tap, const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
	{
		DEBUG_INFO("etherType=%x, len=%d", etherType, len);
		struct pbuf *p,*q;
		if (!tap->_enabled)
			return;
		struct eth_hdr ethhdr;
		from.copyTo(ethhdr.src.addr, 6);
		to.copyTo(ethhdr.dest.addr, 6);
		ethhdr.type = ZeroTier::Utils::hton((uint16_t)etherType);

		p = pbuf_alloc(PBUF_RAW, len+sizeof(struct eth_hdr), PBUF_POOL);
		if (p != NULL) {
			const char *dataptr = reinterpret_cast<const char *>(data);
			// First pbuf gets ethernet header at start
			q = p;
			if (q->len < sizeof(ethhdr)) {
				DEBUG_ERROR("dropped packet: first pbuf smaller than ethernet header");
				return;
			}
			memcpy(q->payload,&ethhdr,sizeof(ethhdr));
			memcpy((char*)q->payload + sizeof(ethhdr),dataptr,q->len - sizeof(ethhdr));
			dataptr += q->len - sizeof(ethhdr);

			// Remaining pbufs (if any) get rest of data
			while ((q = q->next)) {
				memcpy(q->payload,dataptr,q->len);
				dataptr += q->len;
			}
		} 
		else {
			DEBUG_ERROR("dropped packet: no pbufs available");
			return;
		}
		{
#if defined(LIBZT_IPV6)
				if(tap->lwipdev6.input(p, &(tap->lwipdev6)) != ERR_OK) {
					DEBUG_ERROR("error while feeding frame into stack lwipdev6");
				}
#endif
#if defined(LIBZT_IPV4)
				if(tap->lwipdev.input(p, &(tap->lwipdev)) != ERR_OK) {
					DEBUG_ERROR("error while feeding frame into stack lwipdev");
				}
#endif
		}
	}

	int lwIP::lwip_Socket(void **pcb, int socket_family, int socket_type, int protocol)
	{
		DEBUG_INFO();
		if(!can_provision_new_socket()) {
			DEBUG_ERROR("unable to create new socket due to limitation of network stack");
			return -1;
		}
		if(socket_type == SOCK_STREAM) {
			struct tcp_pcb *new_tcp_PCB = tcp_new();
			*pcb = new_tcp_PCB;
			return ERR_OK;
		}
		if(socket_type == SOCK_DGRAM) {
			struct udp_pcb *new_udp_PCB = udp_new();
			*pcb = new_udp_PCB;
			return ERR_OK;
		}
		return -1;
	}

	int lwIP::lwip_Connect(Connection *conn, int fd, const struct sockaddr *addr, socklen_t addrlen)
	{
		DEBUG_INFO();
		ip_addr_t ba;
		char addrstr[INET6_ADDRSTRLEN];
		int port = 0, err = 0;

#if defined(LIBZT_IPV4)
			struct sockaddr_in *in4;
			if(addr->sa_family == AF_INET) {
				in4 = (struct sockaddr_in *)addr;
				inet_ntop(AF_INET, &(in4->sin_addr), addrstr, INET_ADDRSTRLEN); 
				DEBUG_INFO("%s:%d", addrstr, lwip_ntohs(in4->sin_port));
			}
			ba = convert_ip(in4); 
			port = lwip_ntohs(in4->sin_port);
#endif
#if defined(LIBZT_IPV6)
			struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&addr;
			in6_to_ip6((ip6_addr *)&ba, in6);
			if(addr->sa_family == AF_INET6) {        
				struct sockaddr_in6 *connaddr6 = (struct sockaddr_in6 *)addr;
				inet_ntop(AF_INET6, &(connaddr6->sin6_addr), addrstr, INET6_ADDRSTRLEN);
				DEBUG_INFO("%s:%d", addrstr, lwip_ntohs(connaddr6->sin6_port));
			}
#endif

		DEBUG_INFO("addr=%s", addrstr);

		if(conn->socket_type == SOCK_DGRAM) {
			// Generates no network traffic
			if((err = udp_connect((struct udp_pcb*)conn->pcb,(ip_addr_t *)&ba,port)) < 0) {
				DEBUG_ERROR("error while connecting to with UDP");
			}
			udp_recv((struct udp_pcb*)conn->pcb, nc_udp_recved, conn);
			return ERR_OK;
		}

		if(conn->socket_type == SOCK_STREAM) {
			struct tcp_pcb *tpcb = (struct tcp_pcb*)conn->pcb;
			tcp_sent(tpcb, nc_sent);
			tcp_recv(tpcb, nc_recved);
			tcp_err(tpcb, nc_err);
			tcp_poll(tpcb, nc_poll, LWIP_APPLICATION_POLL_FREQ);
			tcp_arg(tpcb, conn);
				
			//DEBUG_EXTRA(" pcb->state=%x", conn->TCP_pcb->state);
			//if(conn->TCP_pcb->state != CLOSED) {
			//	DEBUG_INFO(" cannot connect using this PCB, PCB!=CLOSED");
			//	tap->sendReturnValue(tap->_phy.getDescriptor(rpcSock), -1, EAGAIN);
			//	return;
			//}
			if((err = tcp_connect(tpcb,&ba,port,nc_connected)) < 0)
			{
				if(err == ERR_ISCONN) {
					// Already in connected state
					errno = EISCONN;
					return -1;
				} if(err == ERR_USE) {
					// Already in use
					errno = EADDRINUSE;
					return -1;
				} if(err == ERR_VAL) {
					// Invalid ipaddress parameter
					errno = EINVAL;
					return -1;
				} if(err == ERR_RTE) {
					// No route to host
					errno = ENETUNREACH;
					return -1;
				} if(err == ERR_BUF) {
					// No more ports available
					errno = EAGAIN;
					return -1;
				}
				if(err == ERR_MEM) {
					// TODO: Doesn't describe the problem well, but closest match
					errno = EAGAIN;
					return -1;
				}
				// We should only return a value if failure happens immediately
				// Otherwise, we still need to wait for a callback from lwIP.
				// - This is because an ERR_OK from tcp_connect() only verifies
				//   that the SYN packet was enqueued onto the stack properly,
				//   that's it!
				// - Most instances of a retval for a connect() should happen
				//   in the nc_connect() and nc_err() callbacks!
				DEBUG_ERROR("unable to connect");
				errno = EAGAIN;
				return -1;
			}
		} 
	}

	int lwIP::lwip_Bind(SocketTap *tap, Connection *conn, int fd, const struct sockaddr *addr, socklen_t addrlen)
	{
		DEBUG_INFO();
		ip_addr_t ba;
		char addrstr[INET6_ADDRSTRLEN];
		int port = 0, err = 0;

#if defined(LIBZT_IPV4)
			struct sockaddr_in *in4;
			if(addr->sa_family == AF_INET) {
				in4 = (struct sockaddr_in *)addr;
				inet_ntop(AF_INET, &(in4->sin_addr), addrstr, INET_ADDRSTRLEN); 
				DEBUG_INFO("%s:%d", addrstr, lwip_ntohs(in4->sin_port));
			}
			ba = convert_ip(in4); 
			port = lwip_ntohs(in4->sin_port);
#endif
#if defined(LIBZT_IPV6)
			struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&addr;
			in6_to_ip6((ip6_addr *)&ba, in6);
			if(addr->sa_family == AF_INET6) {        
				struct sockaddr_in6 *connaddr6 = (struct sockaddr_in6 *)addr;
				inet_ntop(AF_INET6, &(connaddr6->sin6_addr), addrstr, INET6_ADDRSTRLEN);
				DEBUG_INFO("%s:%d", addrstr, lwip_ntohs(connaddr6->sin6_port));
			}
#endif
		if(conn->socket_type == SOCK_DGRAM) {
			err = udp_bind((struct udp_pcb*)conn->pcb, (const ip_addr_t *)&ba, port);
			if(err == ERR_USE) {
				err = -1;
				errno = EADDRINUSE; // port in use
			}
			else {
				// set the recv callback
				udp_recv((struct udp_pcb*)conn->pcb, nc_udp_recved, new ConnectionPair(tap, conn));
				err = ERR_OK; 
				errno = ERR_OK; // success
			}
		}
		else if (conn->socket_type == SOCK_STREAM) {
			err = tcp_bind((struct tcp_pcb*)conn->pcb, (const ip_addr_t *)&ba, port);
			if(err != ERR_OK) {
				DEBUG_ERROR("err=%d", err);
				if(err == ERR_USE){
					err = -1; 
					errno = EADDRINUSE;
				}
				if(err == ERR_MEM){
					err = -1; 
					errno = ENOMEM;
				}
				if(err == ERR_BUF){
					err = -1; 
					errno = ENOMEM;
				}
			} 
			else {
				err = ERR_OK; 
				errno = ERR_OK; // success
			}
		}
		return err;
	}

	int lwIP::lwip_Listen(Connection *conn, int backlog)
	{
		DEBUG_INFO("conn=%p", conn);
		struct tcp_pcb* listeningPCB;
#ifdef TCP_LISTEN_BACKLOG
		listeningPCB = tcp_listen_with_backlog((struct tcp_pcb*)conn->pcb, backlog);
#else
		listeningPCB = tcp_listen((struct tcp_pcb*)conn->pcb);
#endif
		if(listeningPCB != NULL) {
			conn->pcb = listeningPCB;
			tcp_accept(listeningPCB, nc_accept); // set callback
			tcp_arg(listeningPCB, conn);
			//fcntl(tap->_phy.getDescriptor(conn->sock), F_SETFL, O_NONBLOCK);
		}
		return 0;
	}

	Connection* lwIP::lwip_Accept(Connection *conn)
	{
		DEBUG_EXTRA("conn=%p", conn);
		if(!conn) {
			DEBUG_ERROR("invalid conn");
			handle_general_failure();
			return NULL;
		}
		// Retreive first of queued Connections from parent connection
		Connection *new_conn = NULL;
		DEBUG_INFO("locking...");
		Mutex::Lock _l(conn->tap->_tcpconns_m);
		DEBUG_INFO("locked.");
		if(conn->_AcceptedConnections.size()) {
			new_conn = conn->_AcceptedConnections.front();
			conn->_AcceptedConnections.pop();
		}
		return new_conn;
	}

	int lwIP::lwip_Read(Connection *conn, bool lwip_invoked)
	{
		DEBUG_EXTRA("conn=%p", conn);
		if(!conn) {
			DEBUG_ERROR("no connection");
			return -1;
		}
		if(!lwip_invoked) {
			DEBUG_INFO("!lwip_invoked");
			conn->tap->_tcpconns_m.lock();
			conn->_rx_m.lock(); 
		}
		if(conn->RXbuf->count()) {
			int max = conn->socket_type == SOCK_STREAM ? ZT_STACK_TCP_SOCKET_RX_SZ : ZT_STACK_TCP_SOCKET_RX_SZ;
			int wr = std::min((ssize_t)max, (ssize_t)conn->RXbuf->count());
			int n = conn->tap->_phy.streamSend(conn->sock, conn->RXbuf->get_buf(), wr);
			char str[22];
			memcpy(str, conn->RXbuf->get_buf(), 22);
			DEBUG_INFO("string = %s", str);
			DEBUG_INFO("n =%d", n);
			conn->RXbuf->consume(n);
			
			//if(n == max) 
			//{
				//if(conn->socket_type == SOCK_DGRAM){
				//	conn->tap->_phy.setNotifyWritable(conn->sock, false);
				//}
			if(conn->socket_type == SOCK_STREAM) { // Only acknolwedge receipt of TCP packets
				tcp_recved((struct tcp_pcb*)conn->pcb, n);
				DEBUG_TRANS("TCP RX %ld bytes", n);
			}
			//}
		}
		if(conn->RXbuf->count() == 0) {
			DEBUG_INFO("wrote everything");
			conn->tap->_phy.setNotifyWritable(conn->sock, false); // nothing else to send to the app
		}
		if(!lwip_invoked) {
			DEBUG_INFO("unlocking...");
			conn->tap->_tcpconns_m.unlock();
			conn->_rx_m.unlock();
		}

		/*
			int payload_sz, addr_sz_offset = sizeof(struct sockaddr_storage);
			memcpy(&payload_sz, conn->rxbuf + addr_sz_offset, sizeof(int)); // OPT:
			// extract address
			struct sockaddr_storage addr;
			memcpy(&addr, conn->rxbuf, addr_sz_offset);
		*/
	}

	int lwIP::lwip_Write(Connection *conn, void *data, ssize_t len)
	{
		DEBUG_EXTRA("conn=%p", (void*)&conn);
		if(!conn) {
			DEBUG_ERROR("no connection");
			return -1;
		}
		if(conn->socket_type == SOCK_DGRAM) 
		{
			// TODO: Packet re-assembly hasn't yet been tested with lwIP so UDP packets are limited to MTU-sized chunks
            int udp_trans_len = std::min((ssize_t)conn->TXbuf->count(), (ssize_t)ZT_MAX_MTU);
			DEBUG_EXTRA("allocating pbuf chain of size=%d for UDP packet, txsz=%d", udp_trans_len, conn->TXbuf->count());
			struct pbuf * pb = pbuf_alloc(PBUF_TRANSPORT, udp_trans_len, PBUF_POOL);
			if(!pb){
				DEBUG_ERROR("unable to allocate new pbuf of size=%d", conn->TXbuf->count());
				return -1;
			}
			memcpy(pb->payload, conn->TXbuf->get_buf(), udp_trans_len);
			int err = udp_send((struct udp_pcb*)conn->pcb, pb);
			
			if(err == ERR_MEM) {
				DEBUG_ERROR("error sending packet. out of memory");
			} else if(err == ERR_RTE) {
				DEBUG_ERROR("could not find route to destinations address");
			} else if(err != ERR_OK) {
				DEBUG_ERROR("error sending packet - %d", err);
			} else {
				conn->TXbuf->consume(udp_trans_len); // success
			}
			pbuf_free(pb);
			return ERR_OK;
		}
		if(conn->socket_type == SOCK_STREAM) 
		{
			// How much we are currently allowed to write to the connection
			ssize_t sndbuf = ((struct tcp_pcb*)conn->pcb)->snd_buf;
			int err, sz, r;
		
			if(!sndbuf) {
				// PCB send buffer is full, turn off readability notifications for the
				// corresponding PhySocket until nc_sent() is called and confirms that there is
				// now space on the buffer
				DEBUG_ERROR(" LWIP stack is full, sndbuf == 0");
				conn->tap->_phy.setNotifyReadable(conn->sock, false);
				return -1;
			}
			if(conn->TXbuf->count() <= 0)
				return -1; // Nothing to write
			
			//if(!conn->listening)
			//	tcp_output(conn->TCP_pcb);

			if(conn->sock) {
				r = std::min((ssize_t)conn->TXbuf->count(), sndbuf);
				// Writes data pulled from the client's socket buffer to LWIP. This merely sends the
				// data to LWIP to be enqueued and eventually sent to the network.
				if(r > 0) {
					err = tcp_write((struct tcp_pcb*)conn->pcb, conn->TXbuf->get_buf(), r, TCP_WRITE_FLAG_COPY);
					tcp_output((struct tcp_pcb*)conn->pcb);
					if(err != ERR_OK) {
						DEBUG_ERROR(" error while writing to PCB, err=%d", err);
						if(err == -1)
							DEBUG_ERROR("out of memory");
						return -1;
					} else {
						conn->TXbuf->consume(r); // success
						return ERR_OK;
					}
				}
			}
		}
	}

	int lwIP::lwip_Close(Connection *conn)
	{
		DEBUG_INFO();

		if(conn->socket_type == SOCK_DGRAM) {
			udp_remove((struct udp_pcb*)conn->pcb);
		}
		// FIXME: check if already closed? conn->TCP_pcb->state != CLOSED
		if(conn->pcb) {
			//DEBUG_EXTRA("conn=%p, sock=%p, PCB->state = %d", 
			//	(void*)&conn, (void*)&sock, conn->TCP_pcb->state);
			if(((struct tcp_pcb*)conn->pcb)->state == SYN_SENT /*|| conn->TCP_pcb->state == CLOSE_WAIT*/) {
				DEBUG_EXTRA("ignoring close request. invalid PCB state for this operation. sock=%p", conn->sock);
				return -1;
			}
			// DEBUG_BLANK("__tcp_close(...)");
			struct tcp_pcb* tpcb = (struct tcp_pcb*)conn->pcb;
			if(tcp_close(tpcb) == ERR_OK) {
				// Unregister callbacks for this PCB
				tcp_arg(tpcb, NULL);
				tcp_recv(tpcb, NULL);
				tcp_err(tpcb, NULL);
				tcp_sent(tpcb, NULL);
				tcp_poll(tpcb, NULL, 1);
			}
			else {
				DEBUG_EXTRA("error while calling tcp_close() sock=%p", conn->sock);
			}
		}
		return 0;
	}

	/****************************************************************************/
	/* Callbacks from lwIP stack                                                */
	/****************************************************************************/

	err_t lwIP::nc_recved(void *arg, struct tcp_pcb *PCB, struct pbuf *p, err_t err)
	{
		DEBUG_INFO();
		Connection *conn = (Connection *)arg;
		int tot = 0;

		if(!conn) {
			DEBUG_ERROR("no connection");
			return ERR_OK; // FIXME: Determine if this is correct behaviour expected by the stack 
		}

		//Mutex::Lock _l(conn->tap->_tcpconns_m);
		//Mutex::Lock _l2(conn->_rx_m);

		DEBUG_INFO("locking...");

		conn->tap->_tcpconns_m.lock();
		conn->_rx_m.lock();

		DEBUG_INFO("locked.");

		struct pbuf* q = p;
		if(p == NULL) {
			if(((struct tcp_pcb*)conn->pcb)->state == CLOSE_WAIT) {
				// FIXME: Implement?
			}
			DEBUG_INFO("p == NULL");
			return ERR_ABRT;
		}

		// Cycle through pbufs and write them to the RX buffer
		// The RX buffer will be emptied via phyOnUnixWritable()
		while(p != NULL) {
			if(p->len <= 0)
				break;
			int avail = ZT_TCP_RX_BUF_SZ - conn->RXbuf->count();
			int len = p->len;
			if(avail < len) {
				DEBUG_ERROR("not enough room (%d bytes) on RX buffer", avail);
			}
			memcpy(conn->RXbuf->get_buf(), p->payload, len);
			conn->RXbuf->produce(len);
			p = p->next;
			tot += len;
		}
		DEBUG_INFO("tot=%d", tot);

		conn->tap->_tcpconns_m.unlock();
		conn->_rx_m.unlock();

		if(tot) {
			conn->tap->_phy.setNotifyWritable(conn->sock, true);
			//conn->tap->phyOnUnixWritable(conn->sock, NULL, true); // to app
		}
		pbuf_free(q);
		return ERR_OK;
	}

	err_t lwIP::nc_accept(void *arg, struct tcp_pcb *newPCB, err_t err)
	{
		Connection *conn = (Connection*)arg;
		DEBUG_INFO("conn=%p", conn);
		//Mutex::Lock _l(conn->tap->_tcpconns_m);
		// create and populate new Connection object
		Connection *new_conn = new Connection();
		new_conn->socket_type = SOCK_STREAM;
		new_conn->pcb = newPCB;
		new_conn->tap = conn->tap;
		new_conn->sock = conn->tap->_phy.wrapSocket(new_conn->sdk_fd, new_conn);
		//memcpy(new_conn->tap->_phy.getuptr(new_conn->sock), new_conn, sizeof(conn));
		DEBUG_INFO("new_conn=%p", new_conn);
		// add new Connection object to parent connection so that we can find it via lwip_Accept()
		conn->_AcceptedConnections.push(new_conn);
		// set callbacks
		tcp_arg(newPCB, new_conn);
		tcp_recv(newPCB, nc_recved);
		tcp_err(newPCB, nc_err);
		tcp_sent(newPCB, nc_sent);
		tcp_poll(newPCB, nc_poll, 1);
		// let lwIP know that it can queue additional incoming connections
		tcp_accepted((struct tcp_pcb*)conn->pcb); 
		return 0;
	}
		
	void lwIP::nc_udp_recved(void * arg, struct udp_pcb * upcb, struct pbuf * p, const ip_addr_t * addr, u16_t port)
	{
		DEBUG_INFO();
		// to be implemented
	}

	err_t lwIP::nc_sent(void* arg, struct tcp_pcb *PCB, u16_t len)
	{
		DEBUG_EXTRA("pcb=%p", (void*)&PCB);
		Connection *conn = (Connection *)arg;
		Mutex::Lock _l(conn->tap->_tcpconns_m);
		if(conn && len) {
			int softmax = conn->socket_type == SOCK_STREAM ? ZT_TCP_TX_BUF_SZ : ZT_UDP_TX_BUF_SZ;
			if(conn->TXbuf->count() < softmax) {
				conn->tap->_phy.setNotifyReadable(conn->sock, true);
				conn->tap->_phy.whack();
			}
		}
		return ERR_OK;
	}

	err_t lwIP::nc_connected(void *arg, struct tcp_pcb *PCB, err_t err)
	{
		DEBUG_ATTN("pcb=%p", (void*)&PCB);
		Connection *conn = (Connection *)arg;
		if(conn)
			return ERR_OK;
		return -1;
		// FIXME: check stack for expected return values
	}

	err_t lwIP::nc_poll(void* arg, struct tcp_pcb *PCB)
	{
		return ERR_OK;
	}

	void lwIP::nc_err(void *arg, err_t err)
	{
		DEBUG_ERROR("err=%d", err);
		Connection *conn = (Connection *)arg;
		Mutex::Lock _l(conn->tap->_tcpconns_m);

		if(!conn){
			DEBUG_ERROR("conn==NULL");
			errno = -1; // FIXME: Find more appropriate value
		}
		int fd = conn->tap->_phy.getDescriptor(conn->sock);
		DEBUG_ERROR("conn=%p, pcb=%p, err=%d", conn, conn->pcb, err);
		DEBUG_ERROR("closing connection");
		conn->tap->Close(conn);
		switch(err)
		{
			case ERR_MEM:
				DEBUG_ERROR("ERR_MEM->ENOMEM");
				errno = ENOMEM;
				break;
			case ERR_BUF:
				DEBUG_ERROR("ERR_BUF->ENOBUFS");
				errno = ENOBUFS;
				break;
			case ERR_TIMEOUT:
				DEBUG_ERROR("ERR_TIMEOUT->ETIMEDOUT");
				errno = ETIMEDOUT;
				break;
			case ERR_RTE:
				DEBUG_ERROR("ERR_RTE->ENETUNREACH");
				errno = ENETUNREACH;
				break;
			case ERR_INPROGRESS:
				DEBUG_ERROR("ERR_INPROGRESS->EINPROGRESS");
				errno = EINPROGRESS;
				break;
			case ERR_VAL:
				DEBUG_ERROR("ERR_VAL->EINVAL");
				errno = EINVAL;
				break;
			case ERR_WOULDBLOCK:
				DEBUG_ERROR("ERR_WOULDBLOCK->EWOULDBLOCK");
				errno = EWOULDBLOCK;
				break;
			case ERR_USE:
				DEBUG_ERROR("ERR_USE->EADDRINUSE");
				errno = EADDRINUSE;
				break;
			case ERR_ISCONN:
				DEBUG_ERROR("ERR_ISCONN->EISCONN");
				errno = EISCONN;
				break;
			case ERR_ABRT:
				DEBUG_ERROR("ERR_ABRT->ECONNREFUSED");
				errno = ECONNREFUSED;
				break;

				// TODO: Below are errors which don't have a standard errno correlate

			case ERR_RST:
				//l->tap->sendReturnValue(fd, -1, -1);
				break;
			case ERR_CLSD:
				//l->tap->sendReturnValue(fd, -1, -1);
				break;
			case ERR_CONN:
				//l->tap->sendReturnValue(fd, -1, -1);
				break;
			case ERR_ARG:
				//l->tap->sendReturnValue(fd, -1, -1);
				break;
			case ERR_IF:
				//l->tap->sendReturnValue(fd, -1, -1);
				break;
			default:
				break;
		}
	}
}