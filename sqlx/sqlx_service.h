/*
OpenIO SDS sqlx
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OIO_SDS__sqlx__sqlx_service_h
# define OIO_SDS__sqlx__sqlx_service_h 1

#define PSRV(P) ((struct sqlx_service_s*)(P))

struct sqlx_service_config_s;
struct sqlx_service_s;

struct election_manager_s;
struct gridd_client_pool_s;
struct gridd_request_dispatcher_s;
struct grid_single_rrd_s;
struct grid_task_queue_s;
struct hc_resolver_s;
struct network_server_s;
struct replication_config_s;
struct service_info_s;
struct sqlx_repo_config_s;
struct sqlx_repository_s;
struct sqlx_sync_s;
struct sqlx_name_s;

struct sqlx_service_config_s
{
	const gchar *srvtype;
	const gchar *srvtag;

	const gchar *zk_prefix;
	const guint zk_hash_depth;
	const guint zk_hash_width;

	const gchar *schema;
	const guint repo_hash_depth;
	const guint repo_hash_width;

	GError* (*get_peers) (struct sqlx_service_s *ss,
			struct sqlx_name_s *n, gboolean nocache,
			gchar ***result);

	// Called at the end of the configure step. Destined to initiating
	// servces backends, plugging message handers, etc.
	gboolean (*post_config) (struct sqlx_service_s *ss);

	// Available to override
	void (*set_defaults) (struct sqlx_service_s *ss);
};

/* Elements common to any gridd+sqliterepo services */
struct sqlx_service_s
{
	gchar volume[1024];
	gchar ns_name[LIMIT_LENGTH_NSNAME];
	struct replication_config_s *replication_config;
	const struct sqlx_service_config_s *service_config;

	GString *url;
	GString *announce;
	gchar *zk_url;

	struct sqlx_repository_s *repository;
	struct sqlx_sync_s *sync;
	struct election_manager_s *election_manager;
	struct network_server_s *server;
	struct gridd_request_dispatcher_s *dispatcher;
	struct hc_resolver_s *resolver;
	struct grid_lbpool_s *lb;

	struct {
		// A queue to transmit events from request workers to the events worker.
		// A pointer to the queue is given to the service backends.
		GAsyncQueue *queue;
		// Runs the converter for GAsyncQueue to ZMQ
		GThread     *th_gq2zmq;
		// Runs the events worker
		GThread     *th_zmq2agent;
		// Context data specific to the ZMQ context
		void        *zctx;
		// Pair of interconnected sockets between the gq2zmq thread and the zmq2agent thread
		void        *zpush; // write only
		void        *zpull; // read only

		/* ZMQ socket to the agent */
		void        *zagent;
		GPtrArray   *pending_events;
		/* used to compute the event id */
		guint16     procid;
		guint       counter;
		/* stats on events streams, managed only by the ZMQ2AGENT thead */
		guint64     counter_received;
		guint64     counter_sent;
		guint64     counter_ack;
		guint64     counter_ack_notfound;

		// how many events are received each time the queue becomes active.
		// A low value helps preventing starvation but leads to more contexts
		// switches.
		guint16     max_recv_per_round;
	} notify;

	// The tasks under this queue always follow a reload of the
	// nsinfo field, and can safely play with it. This is the place
	// for LB reloading, reconfiguration, etc.
	struct namespace_info_s *nsinfo;
	struct grid_task_queue_s *gtq_reload;
	GThread *thread_reload;

	// Queue dedicated to expirations elections, caches, etc.
	struct grid_task_queue_s *gtq_admin;
	GThread *thread_admin;

	// Conscience registration
	struct grid_single_rrd_s *gsr_reqcounter;
	struct grid_single_rrd_s *gsr_reqtime;
	struct service_info_s *si;
	struct grid_task_queue_s *gtq_register;
	GThread *thread_register;

	struct gridd_client_pool_s *clients_pool;
	GThread *thread_client;

	// This is configured during the "configure" step, and can be overriden
	// in the _post_config hook.
	gint64 open_timeout;
	gint64 cnx_backlog;
	guint max_bases;
	guint max_passive;
	guint max_active;

	//-------------------------------------------------------------------
	// Variables used during the startup time of the server, but not used
	// anymore after that.
	//-------------------------------------------------------------------

	GSList *custom_tags;

	guint cfg_max_bases;
	guint cfg_max_passive;
	guint cfg_max_active;
	guint cfg_max_workers;

	guint sync_mode_repli;
	guint sync_mode_solo;

	// Must the cache be set
	gboolean flag_cached_bases;

	// Are DB deletions allowed ?
	gboolean flag_delete_on;

	// Are DB autocreations enabled?
	gboolean flag_autocreate;

	// Turn to TRUE to avoid locking the repository volume
	gboolean flag_nolock;

	// Is the registration task to be executed?
	gboolean flag_noregister;

	// Controls the election mode:
	// TRUE :  ELECTION_MODE_QUORUM
	// FALSE : ELECTION_MODE_NONE
	gboolean flag_replicable;
};

struct meta_backend_common_s {
	gchar ns_name[LIMIT_LENGTH_NSNAME];

	const gchar *type;

	struct sqlx_repository_s *repo;

	// Managed by sqlx_service_extra, do not allocate/free
	struct grid_lbpool_s *lb;
};

/* -------------------------------------------------------------------------- */

extern int sqlite_service_main(int argc, char **argv,
		const struct sqlx_service_config_s *cfg);

/** Enables the optional notification system. To be called during the
 * post-config hook. */
extern gboolean sqlx_enable_notifier (struct sqlx_service_s *ss);

/** Sends a notification. If notifications are not configured, the
 * message is dropped */
extern GError * sqlx_notify (gpointer u, gchar *msg);

/** Reloads the optional (grid_lbpool_s*). Exposed to let the
 * server enable it in its post-config hook. This is destined to
 * be registered in a task queue. */
void sqlx_task_reload_lb(struct sqlx_service_s *ss);

#endif /*OIO_SDS__sqlx__sqlx_service_h*/
