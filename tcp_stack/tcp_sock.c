#include "tcp.h"
#include "tcp_hash.h"
#include "tcp_sock.h"
#include "tcp_timer.h"
#include "ip.h"
#include "rtable.h"
#include "log.h"

#include <time.h>
#include <pthread.h>
#include <unistd.h>

// TCP socks should be hashed into table for later lookup: Those which
// occupy a port (either by *bind* or *connect*) should be hashed into
// bind_table, those which listen for incoming connection request should be
// hashed into listen_table, and those of established connections should
// be hashed into established_table.

struct tcp_hash_table tcp_sock_table;
#define tcp_established_sock_table	tcp_sock_table.established_table
#define tcp_listen_sock_table		tcp_sock_table.listen_table
#define tcp_bind_sock_table			tcp_sock_table.bind_table

inline void tcp_set_state(struct tcp_sock *tsk, int state)
{
	// log(DEBUG, IP_FMT":%hu switch state, from %s to %s.",
	// 		HOST_IP_FMT_STR(tsk->sk_sip), tsk->sk_sport,
	// 		tcp_state_str[tsk->state], tcp_state_str[state]);
	tsk->state = state;
}

// init tcp hash table and tcp timer
void init_tcp_stack()
{
	srand(time(NULL));

	for (int i = 0; i < TCP_HASH_SIZE; i++)
		init_list_head(&tcp_established_sock_table[i]);

	for (int i = 0; i < TCP_HASH_SIZE; i++)
		init_list_head(&tcp_listen_sock_table[i]);

	for (int i = 0; i < TCP_HASH_SIZE; i++)
		init_list_head(&tcp_bind_sock_table[i]);

	pthread_t timer;
	pthread_create(&timer, NULL, tcp_timer_thread, NULL);
}

// allocate tcp sock, and initialize all the variables that can be determined
// now
struct tcp_sock *alloc_tcp_sock()
{
	struct tcp_sock *tsk = malloc(sizeof(struct tcp_sock));

	memset(tsk, 0, sizeof(struct tcp_sock));

	tsk->state = TCP_CLOSED;
	tsk->rcv_wnd = TCP_DEFAULT_WINDOW;
	pthread_mutex_init(&tsk->adv_wnd_lock, NULL);
	pthread_mutex_init(&tsk->rcv_wnd_lock, NULL);

	init_list_head(&tsk->listen_queue);
	init_list_head(&tsk->accept_queue);

	tsk->rcv_buf = alloc_ring_buffer(tsk->rcv_wnd);
	pthread_mutex_init(&tsk->rcv_buf_lock, NULL);

	init_list_head(&tsk->snd_buf);
	init_list_head(&tsk->rcv_ofo_buf);

	tsk->wait_connect = alloc_wait_struct();
	tsk->wait_accept = alloc_wait_struct();
	tsk->wait_recv = alloc_wait_struct();
	tsk->wait_send = alloc_wait_struct();
	tsk->wait_read = alloc_wait_struct();

	u32 iss = tcp_new_iss();
	tsk->snd_una = iss;
	tsk->snd_nxt = iss;

	tsk->cwnd = 1;
	tsk->ssthresh = 0x7fffffff;
	pthread_mutex_init(&tsk->cwnd_lock, NULL);

	return tsk;
}

// release all the resources of tcp sock
//
// To make the stack run safely, each time the tcp sock is refered (e.g. hashed),
// the ref_cnt is increased by 1. each time free_tcp_sock is called, the ref_cnt
// is decreased by 1, and release the resources practically if ref_cnt is
// decreased to zero.
void free_tcp_sock(struct tcp_sock *tsk)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	--tsk->ref_cnt;
	if(tsk->ref_cnt == 0)
	{
		free_ring_buffer(tsk->rcv_buf);
		free_wait_struct(tsk->wait_connect);
		free_wait_struct(tsk->wait_accept);
		free_wait_struct(tsk->wait_recv);
		free_wait_struct(tsk->wait_send);
		free(tsk);
	}
}

// lookup tcp sock in established_table with key (saddr, daddr, sport, dport)
struct tcp_sock *tcp_sock_lookup_established(u32 saddr, u32 daddr, u16 sport, u16 dport)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	int hash = tcp_hash_function(saddr, daddr, sport, dport);
	struct tcp_sock *p;
	list_for_each_entry(p, &tcp_established_sock_table[hash], hash_list)
	{
		if(p->sk_sip == saddr && p->sk_sport == sport &&
		   p->sk_dip == daddr && p->sk_dport == dport)
		{
			return p;
		}
	}

	return NULL;
}

// lookup tcp sock in listen_table with key (sport)
//
// In accordance with BSD socket, saddr is in the argument list, but never used.
struct tcp_sock *tcp_sock_lookup_listen(u32 saddr, u16 sport)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	int hash = tcp_hash_function(0, 0, sport, 0);
	struct tcp_sock *p;
	list_for_each_entry(p, &tcp_listen_sock_table[hash], hash_list)
	{
		if(p->sk_sip == saddr && p->sk_sport == sport)
		{
			return p;
		}
	}

	return NULL;
}

// lookup tcp sock in both established_table and listen_table
struct tcp_sock *tcp_sock_lookup(struct tcp_cb *cb)
{
	u32 saddr = cb->daddr,
		daddr = cb->saddr;
	u16 sport = cb->dport,
		dport = cb->sport;

	struct tcp_sock *tsk = tcp_sock_lookup_established(saddr, daddr, sport, dport);
	if (!tsk)
		tsk = tcp_sock_lookup_listen(saddr, sport);

	return tsk;
}

// hash tcp sock into bind_table, using sport as the key
static int tcp_bind_hash(struct tcp_sock *tsk)
{
	int bind_hash_value = tcp_hash_function(0, 0, tsk->sk_sport, 0);
	struct list_head *list = &tcp_bind_sock_table[bind_hash_value];
	list_add_head(&tsk->bind_hash_list, list);

	tsk->ref_cnt += 1;

	return 0;
}

// unhash the tcp sock from bind_table
void tcp_bind_unhash(struct tcp_sock *tsk)
{
	if (!list_empty(&tsk->bind_hash_list)) {
		list_delete_entry(&tsk->bind_hash_list);
		free_tcp_sock(tsk);
	}
}

// lookup bind_table to check whether sport is in use
static int tcp_port_in_use(u16 sport)
{
	int value = tcp_hash_function(0, 0, sport, 0);
	struct list_head *list = &tcp_bind_sock_table[value];
	struct tcp_sock *tsk;
	list_for_each_entry(tsk, list, hash_list) {
		if (tsk->sk_sport == sport)
			return 1;
	}

	return 0;
}

// find a free port by looking up bind_table
static u16 tcp_get_port()
{
	for (u16 port = PORT_MIN; port < PORT_MAX; port++) {
		if (!tcp_port_in_use(port))
			return port;
	}

	return 0;
}

// tcp sock tries to use port as its source port
int tcp_sock_set_sport(struct tcp_sock *tsk, u16 port)
{
	if ((port && tcp_port_in_use(port)) ||
			(!port && !(port = tcp_get_port())))
		return -1;

	tsk->sk_sport = port;

	tcp_bind_hash(tsk);

	return 0;
}

// hash tcp sock into either established_table or listen_table according to its
// TCP_STATE
int tcp_hash(struct tcp_sock *tsk)
{
	struct list_head *list;
	int hash;

	if (tsk->state == TCP_CLOSED)
		return -1;

	if (tsk->state == TCP_LISTEN) {
		hash = tcp_hash_function(0, 0, tsk->sk_sport, 0);
		list = &tcp_listen_sock_table[hash];
	}
	else {
		hash = tcp_hash_function(tsk->sk_sip, tsk->sk_dip, \
				tsk->sk_sport, tsk->sk_dport);
		list = &tcp_established_sock_table[hash];

		struct tcp_sock *tmp;
		list_for_each_entry(tmp, list, hash_list) {
			if (tsk->sk_sip == tmp->sk_sip &&
					tsk->sk_dip == tmp->sk_dip &&
					tsk->sk_sport == tmp->sk_sport &&
					tsk->sk_dport == tmp->sk_dport)
				return -1;
		}
	}

	list_add_head(&tsk->hash_list, list);
	tsk->ref_cnt += 1;

	return 0;
}

// unhash tcp sock from established_table or listen_table
void tcp_unhash(struct tcp_sock *tsk)
{
	if (!list_empty(&tsk->hash_list)) {
		list_delete_entry(&tsk->hash_list);
		free_tcp_sock(tsk);
	}
}

// XXX: skaddr here contains network-order variables
int tcp_sock_bind(struct tcp_sock *tsk, struct sock_addr *skaddr)
{
	int err = 0;

	// omit the ip address, and only bind the port
	err = tcp_sock_set_sport(tsk, ntohs(skaddr->port));

	return err;
}

void *print_cwnd(void *arg)
{
	FILE *f = fopen("./cwnd.txt", "w");
	struct tcp_sock *t = *((struct tcp_sock **)arg);
	while(1)
	{
		usleep(10000);
		pthread_mutex_lock(&t->cwnd_lock);
		fprintf(f, "%d\n", t->cwnd);
		fflush(f);
		pthread_mutex_unlock(&t->cwnd_lock);
	}
	return NULL;
}

// connect to the remote tcp sock specified by skaddr
//
// XXX: skaddr here contains network-order variables
// 1. initialize the four key tuple (sip, sport, dip, dport);
// 2. hash the tcp sock into bind_table;
// 3. send SYN packet, switch to TCP_SYN_SENT state, wait for the incoming
//    SYN packet by sleep on wait_connect;
// 4. if the SYN packet of the peer arrives, this function is notified, which
//    means the connection is established.
int tcp_sock_connect(struct tcp_sock *tsk, struct sock_addr *skaddr)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	iface_info_t *iface = list_entry(instance->iface_list.next, iface_info_t, list);
	tsk->sk_sip = iface->ip;
	if(tcp_sock_set_sport(tsk, 0) == -1)
		return -1;
	tsk->sk_dip = ntohl(skaddr->ip);
	tsk->sk_dport = ntohs(skaddr->port);
	//pthread_t id;
	//pthread_create(&id, NULL, print_cwnd, &tsk);
	tcp_send_control_packet(tsk, TCP_SYN);
	tcp_set_state(tsk, TCP_SYN_SENT);
	if(tcp_hash(tsk) == -1)
		return -1;
	sleep_on(tsk->wait_connect);
	return 0;
}

// set backlog (the maximum number of pending connection requst), switch the
// TCP_STATE, and hash the tcp sock into listen_table
int tcp_sock_listen(struct tcp_sock *tsk, int backlog)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	tsk->backlog = backlog;
	iface_info_t *iface = list_entry(instance->iface_list.next, iface_info_t, list);
	tsk->sk_sip = iface->ip;
	tcp_set_state(tsk, TCP_LISTEN);
	return tcp_hash(tsk);
}

// check whether the accept queue is full
inline int tcp_sock_accept_queue_full(struct tcp_sock *tsk)
{
	if (tsk->accept_backlog >= tsk->backlog) {
		log(ERROR, "tcp accept queue (%d) is full.", tsk->accept_backlog);
		return 1;
	}

	return 0;
}

// push the tcp sock into accept_queue
inline void tcp_sock_accept_enqueue(struct tcp_sock *tsk)
{
	if (!list_empty(&tsk->list))
		list_delete_entry(&tsk->list);
	list_add_tail(&tsk->list, &tsk->parent->accept_queue);
	tsk->parent->accept_backlog += 1;
}

// pop the first tcp sock of the accept_queue
inline struct tcp_sock *tcp_sock_accept_dequeue(struct tcp_sock *tsk)
{
	struct tcp_sock *new_tsk = list_entry(tsk->accept_queue.next, struct tcp_sock, list);
	list_delete_entry(&new_tsk->list);
	init_list_head(&new_tsk->list);
	tsk->accept_backlog -= 1;

	return new_tsk;
}

// if accept_queue is not emtpy, pop the first tcp sock and accept it,
// otherwise, sleep on the wait_accept for the incoming connection requests
struct tcp_sock *tcp_sock_accept(struct tcp_sock *tsk)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	if(list_empty(&tsk->accept_queue))
		sleep_on(tsk->wait_accept);

	return tcp_sock_accept_dequeue(tsk);
}

// close the tcp sock, by releasing the resources, sending FIN/RST packet
// to the peer, switching TCP_STATE to closed
void tcp_sock_close(struct tcp_sock *tsk)
{
	// fprintf(stdout, "TODO: implement %s please.\n", __FUNCTION__);

	tcp_send_control_packet(tsk, TCP_FIN | TCP_ACK);
	if(tsk->state == TCP_ESTABLISHED)
		tcp_set_state(tsk, TCP_FIN_WAIT_1);
	else
		tcp_set_state(tsk, TCP_LAST_ACK);
}

int tcp_sock_write(struct tcp_sock *tsk, char *buf, int len)
{
	if(tsk->state != TCP_ESTABLISHED)
		return -1;
	int remain = len, tot_len = len;
	int appended = TCP_BASE_HDR_SIZE + IP_BASE_HDR_SIZE + ETHER_HDR_SIZE;
	int inflight, snd_wnd;
	pthread_mutex_lock(&tsk->adv_wnd_lock);
	while(remain)
	{
		len = (remain < MSS) ? remain : MSS;
		inflight = (tsk->snd_nxt - tsk->snd_una);
		snd_wnd = (tsk->adv_wnd > tsk->cwnd*MSS) ? tsk->cwnd*MSS : tsk->adv_wnd;
		while(snd_wnd - inflight < len)
		{
			pthread_mutex_unlock(&tsk->adv_wnd_lock);
			sleep_on(tsk->wait_send);
			pthread_mutex_lock(&tsk->adv_wnd_lock);
			inflight = (tsk->snd_nxt - tsk->snd_una);
			snd_wnd = (tsk->adv_wnd > tsk->cwnd*MSS) ? tsk->cwnd*MSS : tsk->adv_wnd;
		}
		u32 plen = len + appended;
		char *packet = malloc(plen);
		if(!packet)
			return -1;
		memcpy(packet + ETHER_HDR_SIZE + IP_BASE_HDR_SIZE + TCP_BASE_HDR_SIZE, buf, len);
		buf += len;
		tcp_send_packet(tsk, packet, plen);
		remain -= len;
	}
	pthread_mutex_unlock(&tsk->adv_wnd_lock);
	return tot_len;
}

int tcp_sock_read(struct tcp_sock *tsk, char *buf, int len)
{
	pthread_mutex_lock(&tsk->rcv_buf_lock);
	if(ring_buffer_empty(tsk->rcv_buf))
	{
		pthread_mutex_unlock(&tsk->rcv_buf_lock);
		if(tsk->state != TCP_ESTABLISHED)
			return 0;
		else
			sleep_on(tsk->wait_recv);
		pthread_mutex_lock(&tsk->rcv_buf_lock);
	}
	int used = ring_buffer_used(tsk->rcv_buf);
	if(len > used)
		len = used;
	read_ring_buffer(tsk->rcv_buf, buf, len);
	pthread_mutex_unlock(&tsk->rcv_buf_lock);
	pthread_mutex_lock(&tsk->rcv_wnd_lock);
	tsk->rcv_wnd += len;
	if(tsk->rcv_wnd - len < MSS)
		tcp_send_control_packet(tsk, TCP_ACK);
	pthread_mutex_unlock(&tsk->rcv_wnd_lock);
	wake_up(tsk->wait_read);
	return len;
}
