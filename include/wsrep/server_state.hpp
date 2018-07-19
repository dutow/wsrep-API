//
// Copyright (C) 2018 Codership Oy <info@codership.com>
//

/** @file server_state.hpp
 *
 * Server State Abstraction
 * ==========================
 *
 * This file defines an interface for WSREP Server State.
 * The Server State t will encapsulate server identification,
 * server state and server capabilities. The class also
 * defines an interface for manipulating server state, applying
 * of remote transaction write sets, processing SST requests,
 * creating local client connections for local storage access
 * operations.
 *
 * Concepts
 * ========
 *
 * State Snapshot Transfer
 * -----------------------
 *
 * Depending on SST type (physical or logical), the server storage
 * engine initialization must be done before or after SST happens.
 * In case of physical SST method (typically rsync, filesystem snapshot)
 * the SST happens before the storage engine is initialized, in case
 * of logical backup typically after the storage engine initialization.
 *
 * Rollback Mode
 * -------------
 *
 * When High Prioity Transaction (HTP) write set is applied, it
 * may be required that the HTP Brute Force Aborts (BFA) locally
 * executing transaction. As HTP must be able to apply all its
 * write sets without interruption, the locally executing transaction
 * must yield immediately, otherwise a transaction processing
 * may stop or even deadlock. Depending on DBMS implementation,
 * the local transaction may need to be rolled back immediately
 * (synchronous mode)  or the rollback may happen later on
 * (asynchronous mode). The Server Context implementation
 * which derives from Server Context base class must provide
 * the base class the rollback mode which server operates on.
 *
 * ### Synchronous
 *
 * If the DBMS server implementation does not allow asynchronous rollback,
 * the victim transaction must be rolled back immediately in order to
 * allow transaction processing to proceed. Depending on DBMS process model,
 * there may be either background thread which processes the rollback
 * or the rollback can be done by the HTP applier.
 *
 * ### Asynchronous
 *
 * In asynchronous mode the BFA victim transaction is just marked
 * to be aborted or in case of fully optimistic concurrency control,
 * the conflict is detected at commit.
 *
 *
 * # Return value conventions
 *
 * The calls which are proxies to corresponding provider functionality
 * will return wsrep::provider::status enum as a result. Otherwise
 * the return value is generally zero on success, non zero on failure.
 */

#ifndef WSREP_SERVER_STATE_HPP
#define WSREP_SERVER_STATE_HPP

#include "mutex.hpp"
#include "condition_variable.hpp"
#include "id.hpp"
#include "view.hpp"
#include "transaction_id.hpp"
#include "provider.hpp"

#include <vector>
#include <string>
#include <map>

namespace wsrep
{
    // Forward declarations
    class ws_handle;
    class ws_meta;
    class client_state;
    class transaction;
    class const_buffer;
    class server_service;

    /** @class Server Context
     *
     *
     */
    class server_state
    {
    public:
        /**
         * Server state enumeration.
         *
         * @todo Fix UML generation
         *
         * Server state diagram if the sst_before_init() returns false.
         *
         * [*] --> disconnected
         * disconnected --> initializing
         * initializing --> initialized
         * initialized --> connected
         * connected --> joiner
         * joiner --> joined
         * joined --> synced
         * synced --> donor
         * donor --> joined
         *
         * Server state diagram if the sst_before_init() returns true.
         *
         * [*] --> disconnected
         * disconnected --> connected
         * connected --> joiner
         * joiner --> initializing
         * initializing --> initialized
         * initialized --> joined
         * joined --> synced
         * synced --> donor
         * donor --> joined
         */
        enum state
        {
            /** Server is in disconnected state. */
            s_disconnected,
            /** Server is initializing */
            s_initializing,
            /** Server has been initialized */
            s_initialized,
            /** Server is connected to the cluster */
            s_connected,
            /** Server is receiving SST */
            s_joiner,
            /** Server has received SST succesfully but has not synced
              with rest of the cluster yet. */
            s_joined,
            /** Server is donating state snapshot transfer */
            s_donor,
            /** Server has synced with the cluster */
            s_synced,
            /** Server is disconnecting from group */
            s_disconnecting
        };

        static const int n_states_ = s_disconnecting + 1;

        /**
         * Rollback Mode enumeration
         */
        enum rollback_mode
        {
            /** Asynchronous rollback mode */
            rm_async,
            /** Synchronous rollback mode */
            rm_sync
        };


        virtual ~server_state();


        wsrep::server_service& server_service() { return server_service_; }
        /**
         * Return human readable server name.
         *
         * @return Human readable server name string.
         */
        const std::string& name() const { return name_; }

        /**
         * Return Server identifier.
         *
         * @return Server identifier.
         */
        const wsrep::id& id() const { return id_; }

        const std::string& incoming_address() const
        { return incoming_address_; }
        /**
         * Return server group communication address.
         *
         * @return Return server group communication address.
         */
        const std::string& address() const { return address_; }

        /**
         * Return working directory
         *
         * @return String containing path to working directory.
         */
        const std::string& working_dir() const { return working_dir_; }

        /**
         * Return initial position for server.
         */
        const wsrep::gtid& initial_position() const
        { return initial_position_; }
        /**
         * Return maximum protocol version.
         */
        int max_protocol_version() const { return max_protocol_version_;}

        /**
         * Get the rollback mode which server is operating in.
         *
         * @return Rollback mode.
         */
        enum rollback_mode rollback_mode() const { return rollback_mode_; }

        /**
         * Registers a streaming client.
         */
        void start_streaming_client(wsrep::client_state* client_state);

        void convert_streaming_client_to_applier(
            wsrep::client_state* client_state);
        void stop_streaming_client(wsrep::client_state* client_state);

        void start_streaming_applier(
            const wsrep::id&,
            const wsrep::transaction_id&,
            wsrep::high_priority_service*);

        void stop_streaming_applier(
            const wsrep::id&, const wsrep::transaction_id&);
        /**
         * Return reference to streaming applier.
         */
        wsrep::high_priority_service* find_streaming_applier(
            const wsrep::id&,
            const wsrep::transaction_id&) const;
        /**
         * Load WSRep provider.
         *
         * @param provider WSRep provider library to be loaded.
         * @param provider_options Provider specific options string
         *        to be passed for provider during initialization.
         *
         * @return Zero on success, non-zero on error.
         */
        int load_provider(const std::string& provider,
                          const std::string& provider_options);

        void unload_provider();

        /**
         * Return reference to provider.
         *
         * @return Reference to provider
         *
         * @throw wsrep::runtime_error if provider has not been loaded
         *
         * @todo This should not be virtual.
         */
        virtual wsrep::provider& provider() const
        {
            if (provider_ == 0)
            {
                throw wsrep::runtime_error("provider not loaded");
            }
            return *provider_;
        }

        int connect(const std::string& cluster_name,
                    const std::string& cluster_address,
                    const std::string& state_donor,
                    bool bootstrap);

        int disconnect();

        /**
         * A method which will be called when the server
         * has been joined to the cluster
         */
        void on_connect(const wsrep::gtid& gtid);

        /**
         * A method which will be called when a view
         * notification event has been delivered by the
         * provider.
         *
         * @params view wsrep::view object which holds the new view
         *         information.
         */
        void on_view(const wsrep::view& view,
                     wsrep::high_priority_service*);

        /**
         * A method which will be called when the server
         * has been synchronized with the cluster.
         *
         * This will have a side effect of changing the Server Context
         * state to s_synced.
         */
        void on_sync();

        /**
         * Wait until server reaches given state.
         */
        void wait_until_state(enum state state) const
        {
            wsrep::unique_lock<wsrep::mutex> lock(mutex_);
            wait_until_state(lock, state);
        }

        /**
         * Return GTID at the position when server connected to
         * the cluster.
         */
        wsrep::gtid connected_gtid() const { return connected_gtid_; }

        /**
         *  Return current view
         */
        const wsrep::view& current_view() const { return current_view_; }
        /**
         * Set last committed GTID.
         */
        void last_committed_gtid(const wsrep::gtid&);
        /**
         * Return the last committed GTID known to be committed
         * on server.
         */
        wsrep::gtid last_committed_gtid() const;

        /**
         * Wait until all the write sets up to given GTID have been
         * committed.
         *
         * @return Zero on success, non-zero on failure.
         */
        enum wsrep::provider::status
        wait_for_gtid(const wsrep::gtid&, int timeout) const;

        /**
         * Perform a causal read in the cluster. After the call returns,
         * all the causally preceding write sets have been committed
         * or the error is returned.
         *
         * This operation may require communication with other processes
         * in the DBMS cluster, so it may be relatively heavy operation.
         * Method wait_for_gtid() should be used whenever possible.
         *
         * @param timeout Timeout in seconds
         *
         * @return Pair of GTID and result status from provider.
         */

        std::pair<wsrep::gtid, enum wsrep::provider::status>
        causal_read(int timeout) const;

        /**
         * Desynchronize the server.
         *
         * If the server state is synced, this call will desynchronize
         * the server from the cluster.
         */
        int desync()
        {
            wsrep::unique_lock<wsrep::mutex> lock(mutex_);
            return desync(lock);
        }

        /**
         * Resynchronize the server.
         */
        void resync()
        {
            wsrep::unique_lock<wsrep::mutex> lock(mutex_);
            resync(lock);
        }

        wsrep::seqno pause();

        wsrep::seqno pause_seqno() const { return pause_seqno_; }

        void resume();

        /**
         * Desync and pause the provider on one go. Will return
         * pause seqno if successful. In case of failure,
         * undefined seqno will be returned.
         */
        wsrep::seqno desync_and_pause();

        /**
         * Resume and resync the provider on one go. Prior this
         * call the provider must have been both desynced and paused,
         * by either desync_and_pause() or separate calls to desync()
         * and pause().
         */
        void resume_and_resync();

        /**
         * Prepares server state for SST.
         *
         * @return String containing a SST request
         */
        std::string prepare_for_sst();

        /**
         * Start a state snapshot transfer.
         *
         * @param sst_requets SST request string
         * @param gtid Current GTID
         * @param bypass Bypass flag
         *
         * @return Zero in case of success, non-zero otherwise
         */
        int start_sst(const std::string& sst_request,
                      const wsrep::gtid& gtid,
                      bool bypass);

        /**
         *
         */
        void sst_sent(const wsrep::gtid& gtid, int error);

        /**
         * This method must be called by the joiner after the SST
         * transfer has been received. If the DBMS state has not been
         * initialized, the call will shift the state to initializing
         * and will wait until the initialization is complete.
         *
         * @param gtid GTID provided by the SST transfer
         */
        void sst_received(const wsrep::gtid& gtid, int error);

        /**
         * This method must be called after the server initialization
         * has been completed. The call has a side effect of changing
         * the Server Context state to s_initialized.
         */
        void initialized();

        /**
         * Return true if the server has been initialized.
         */
        bool is_initialized() const
        {
            wsrep::unique_lock<wsrep::mutex> lock(mutex_);
            return init_initialized_;
        }
        /**
         *
         */
        /**
         * This method will be called by the provider hen
         * a remote write set is being applied. It is the responsibility
         * of the caller to set up transaction context and data properly.
         *
         * @todo Make this private, allow calls for provider implementations
         *       only.
         * @param high_priority_service High priority applier service.
         * @param transaction Transaction context.
         * @param data Write set data
         *
         * @return Zero on success, non-zero on failure.
         */
        int on_apply(wsrep::high_priority_service& high_priority_service,
                     const wsrep::ws_handle& ws_handle,
                     const wsrep::ws_meta& ws_meta,
                     const wsrep::const_buffer& data);

        enum state state() const
        {
            wsrep::unique_lock<wsrep::mutex> lock(mutex_);
            return state(lock);
        }

        enum state state(wsrep::unique_lock<wsrep::mutex>& lock) const
        {
            assert(lock.owns_lock());
            return state_;
        }

        /**
         * Get provider status variables.
         */
        std::vector<wsrep::provider::status_variable> status() const;

        /**
         * Set server wide wsrep debug logging level.
         *
         * Log levels are
         * - 0 - No debug logging.
         * - 1..n - Debug logging with increasing verbosity.
         */
        void debug_log_level(int level) { debug_log_level_ = level; }

        /**
         *
         */
        int debug_log_level() const { return debug_log_level_; }

        /**
         * @todo Set filter for debug logging.
         */
        void debug_log_filter(const std::string&);

        wsrep::mutex& mutex() { return mutex_; }

    protected:
        /** Server state constructor
         *
         * @param mutex Mutex provided by the DBMS implementation.
         * @param name Human Readable Server Name.
         * @param id Server Identifier String, UUID or some unique
         *        identifier.
         * @param address Server address in form of IPv4 address, IPv6 address
         *        or hostname.
         * @param working_dir Working directory for replication specific
         *        data files.
         * @param rollback_mode Rollback mode which server operates on.
         */
        server_state(wsrep::mutex& mutex,
                     wsrep::condition_variable& cond,
                     wsrep::server_service& server_service,
                     const std::string& name,
                     const std::string& id,
                     const std::string& incoming_address,
                     const std::string& address,
                     const std::string& working_dir,
                     const wsrep::gtid& initial_position,
                     int max_protocol_version,
                     enum rollback_mode rollback_mode)
            : mutex_(mutex)
            , cond_(cond)
            , server_service_(server_service)
            , state_(s_disconnected)
            , state_hist_()
            , state_waiters_(n_states_)
            , bootstrap_()
            , initial_position_(initial_position)
            , init_initialized_()
            , init_synced_()
            , sst_gtid_()
            , desync_count_()
            , pause_count_()
            , pause_seqno_()
            , streaming_clients_()
            , streaming_appliers_()
            , provider_()
            , name_(name)
            , id_(id)
            , incoming_address_(incoming_address)
            , address_(address)
            , working_dir_(working_dir)
            , max_protocol_version_(max_protocol_version)
            , rollback_mode_(rollback_mode)
            , connected_gtid_()
            , current_view_()
            , last_committed_gtid_()
            , debug_log_level_(0)
        { }

    private:

        server_state(const server_state&);
        server_state& operator=(const server_state&);

        int desync(wsrep::unique_lock<wsrep::mutex>&);
        void resync(wsrep::unique_lock<wsrep::mutex>&);
        void state(wsrep::unique_lock<wsrep::mutex>&, enum state);
        void wait_until_state(wsrep::unique_lock<wsrep::mutex>&, enum state) const;
        // Close SR transcations whose origin is outside of current
        // cluster view.
        void close_foreign_sr_transactions(
            wsrep::unique_lock<wsrep::mutex>&,
            wsrep::high_priority_service&);

        // Close transactions when handling disconnect from the group.
        void close_transactions_at_disconnect(wsrep::high_priority_service&);

        wsrep::mutex& mutex_;
        wsrep::condition_variable& cond_;
        wsrep::server_service& server_service_;
        enum state state_;
        std::vector<enum state> state_hist_;
        mutable std::vector<int> state_waiters_;
        bool bootstrap_;
        const wsrep::gtid initial_position_;
        bool init_initialized_;
        bool init_synced_;
        wsrep::gtid sst_gtid_;
        size_t desync_count_;
        size_t pause_count_;
        wsrep::seqno pause_seqno_;
        typedef std::map<wsrep::client_id, wsrep::client_state*>
        streaming_clients_map;
        streaming_clients_map streaming_clients_;
        typedef std::map<std::pair<wsrep::id, wsrep::transaction_id>,
                         wsrep::high_priority_service*> streaming_appliers_map;
        class server_id_cmp
        {
        public:
            server_id_cmp(const wsrep::id& server_id)
                : server_id_(server_id)
            { }
            bool operator()(const std::vector<wsrep::view::member>::value_type& vt) const
            {
                return (vt.id() == server_id_);
            }
        private:
            wsrep::id server_id_;
        };

        streaming_appliers_map streaming_appliers_;
        wsrep::provider* provider_;
        std::string name_;
        wsrep::id id_;
        std::string incoming_address_;
        std::string address_;
        std::string working_dir_;
        int max_protocol_version_;
        enum rollback_mode rollback_mode_;
        wsrep::gtid connected_gtid_;
        wsrep::view current_view_;
        wsrep::gtid last_committed_gtid_;
        int debug_log_level_;
    };


    static inline const char* to_c_string(
        enum wsrep::server_state::state state)
    {
        switch (state)
        {
        case wsrep::server_state::s_disconnected:  return "disconnected";
        case wsrep::server_state::s_initializing:  return "initializing";
        case wsrep::server_state::s_initialized:   return "initialized";
        case wsrep::server_state::s_connected:     return "connected";
        case wsrep::server_state::s_joiner:        return "joiner";
        case wsrep::server_state::s_joined:        return "joined";
        case wsrep::server_state::s_donor:         return "donor";
        case wsrep::server_state::s_synced:        return "synced";
        case wsrep::server_state::s_disconnecting: return "disconnecting";
        }
        return "unknown";
    }

    static inline std::string to_string(enum wsrep::server_state::state state)
    {
        return (to_c_string(state));
    }

}

#endif // WSREP_SERVER_STATE_HPP
