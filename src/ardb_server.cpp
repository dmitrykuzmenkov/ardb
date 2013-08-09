/*
 *Copyright (c) 2013-2013, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 * 
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 * 
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * 
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS 
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ardb_server.hpp"
#include "stat.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>
#include <fnmatch.h>
#include <sstream>

namespace ardb
{
	static ServerStat kServerStat;

	static inline void fill_error_reply(RedisReply& reply, const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		char buf[1024];
		vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
		va_end(ap);
		reply.type = REDIS_REPLY_ERROR;
		reply.str = buf;
	}

	static inline void fill_status_reply(RedisReply& reply, const char* s)
	{
		reply.type = REDIS_REPLY_STATUS;
		reply.str = s;
	}

	static inline void fill_int_reply(RedisReply& reply, int64 v)
	{
		reply.type = REDIS_REPLY_INTEGER;
		reply.integer = v;
	}
	static inline void fill_double_reply(RedisReply& reply, double v)
	{
		reply.type = REDIS_REPLY_DOUBLE;
		reply.double_value = v;
	}

	static inline void fill_str_reply(RedisReply& reply, const std::string& v)
	{
		reply.type = REDIS_REPLY_STRING;
		reply.str = v;
	}

	static bool check_uint32_arg(RedisReply& reply, const std::string& arg,
			uint32& v)
	{
		if (!string_touint32(arg, v))
		{
			fill_error_reply(reply,
					"ERR value is not an integer or out of range.");
			return false;
		}
		return true;
	}

	static bool check_uint64_arg(RedisReply& reply, const std::string& arg,
			uint64& v)
	{
		if (!string_touint64(arg, v))
		{
			fill_error_reply(reply,
					"ERR value is not an integer or out of range.");
			return false;
		}
		return true;
	}

	template<typename T>
	static inline void fill_array_reply(RedisReply& reply, T& v)
	{
		reply.type = REDIS_REPLY_ARRAY;
		typename T::iterator it = v.begin();
		while (it != v.end())
		{
			const ValueObject& vo = *it;
			RedisReply r;
			if (vo.type == EMPTY)
			{
				r.type = REDIS_REPLY_NIL;
			} else
			{
				std::string str;
				vo.ToString(str);
				fill_str_reply(r, str);
			}
			reply.elements.push_back(r);
			it++;
		}
	}

	template<typename T>
	static inline void fill_str_array_reply(RedisReply& reply, T& v)
	{
		reply.type = REDIS_REPLY_ARRAY;
		typename T::iterator it = v.begin();
		while (it != v.end())
		{
			RedisReply r;
			fill_str_reply(r, *it);
			reply.elements.push_back(r);
			it++;
		}
	}

	template<typename T>
	static inline void fill_int_array_reply(RedisReply& reply, T& v)
	{
		reply.type = REDIS_REPLY_ARRAY;
		typename T::iterator it = v.begin();
		while (it != v.end())
		{
			RedisReply r;
			fill_int_reply(r, *it);
			reply.elements.push_back(r);
			it++;
		}
	}

	int ArdbServer::ParseConfig(const Properties& props, ArdbServerConfig& cfg)
	{
		conf_get_string(props, "home", cfg.home);
		if (cfg.home.empty())
		{
			cfg.home = "../ardb";
		}
		setenv("ARDB_HOME", cfg.home.c_str(), 1);
		replace_env_var(const_cast<Properties&>(props));

		conf_get_int64(props, "port", cfg.listen_port);
		conf_get_int64(props, "tcp-keepalive", cfg.tcp_keepalive);
		conf_get_int64(props, "timeout", cfg.timeout);
		conf_get_int64(props, "unixsocketperm", cfg.unixsocketperm);
		conf_get_int64(props, "slowlog-log-slower-than",
				cfg.slowlog_log_slower_than);
		conf_get_int64(props, "slowlog-max-len", cfg.slowlog_max_len);
		conf_get_int64(props, "maxclients", cfg.max_clients);
		conf_get_string(props, "bind", cfg.listen_host);
		conf_get_string(props, "unixsocket", cfg.listen_unix_path);

		conf_get_string(props, "data-dir", cfg.data_base_path);
		conf_get_string(props, "backup-dir", cfg.backup_dir);
		conf_get_string(props, "repl-dir", cfg.repl_data_dir);
		conf_get_string(props, "loglevel", cfg.loglevel);
		conf_get_string(props, "logfile", cfg.logfile);
		std::string daemonize, repl_log_enable;
		conf_get_string(props, "daemonize", daemonize);
		conf_get_string(props, "repl-log-enable", repl_log_enable);

		conf_get_int64(props, "thread-pool-size", cfg.worker_count);
		if (cfg.worker_count <= 0)
		{
			cfg.worker_count = available_processors();
		}
		conf_get_int64(props, "repl-backlog-size", cfg.repl_backlog_size);
		conf_get_int64(props, "repl-ping-slave-period",
				cfg.repl_ping_slave_period);
		conf_get_int64(props, "repl-timeout", cfg.repl_timeout);
		conf_get_int64(props, "repl-sync-state-persist-period",
				cfg.repl_syncstate_persist_period);
		conf_get_int64(props, "repl-max-backup-logs", cfg.repl_max_backup_logs);

		std::string slaveof;
		if (conf_get_string(props, "slaveof", slaveof))
		{
			std::vector<std::string> ss = split_string(slaveof, ":");
			if (ss.size() == 2)
			{
				cfg.master_host = ss[0];
				if (!string_touint32(ss[1], cfg.master_port))
				{
					cfg.master_host = "";
					WARN_LOG("Invalid 'slaveof' config.");
				}
				if (cfg.master_port == cfg.listen_port
						&& is_local_ip(cfg.master_host))
				{
					cfg.master_host = "";
					WARN_LOG("Invalid 'slaveof' config.");
				}
			} else
			{
				WARN_LOG("Invalid 'slaveof' config.");
			}
		}

		std::string syncdbs;
		if (conf_get_string(props, "syncdb", syncdbs))
		{
			cfg.syncdbs.clear();
			std::vector<std::string> ss = split_string(syncdbs, "|");
			for (uint32 i = 0; i < ss.size(); i++)
			{
				DBID id;
				if (!string_touint32(ss[i], id))
				{
					ERROR_LOG("Invalid 'syncdb' config.");
				} else
				{
					cfg.syncdbs.insert(id);
				}
			}
		}

		daemonize = string_tolower(daemonize);
		if (daemonize == "yes")
		{
			cfg.daemonize = true;
		}
		if (repl_log_enable == "no")
		{
			cfg.repl_log_enable = false;
		}
		if (cfg.data_base_path.empty())
		{
			cfg.data_base_path = ".";
		}
		ArdbLogger::SetLogLevel(cfg.loglevel);
		return 0;
	}

	ArdbServer::ArdbServer(KeyValueEngineFactory& engine) :
			m_service(NULL), m_db(NULL), m_engine(engine), m_slowlog_handler(
					m_cfg), m_repli_serv(this), m_slave_client(this), m_watch_mutex(
			PTHREAD_MUTEX_RECURSIVE)
	{
		struct RedisCommandHandlerSetting settingTable[] =
			{
				{ "ping", REDIS_CMD_PING, &ArdbServer::Ping, 0, 0, 0 },
				{ "multi", REDIS_CMD_MULTI, &ArdbServer::Multi, 0, 0, 0 },
				{ "discard", REDIS_CMD_DISCARD, &ArdbServer::Discard, 0, 0, 0 },
				{ "exec", REDIS_CMD_EXEC, &ArdbServer::Exec, 0, 0, 0 },
				{ "watch", REDIS_CMD_WATCH, &ArdbServer::Watch, 0, -1, 0 },
				{ "unwatch",REDIS_CMD_UNWATCH, &ArdbServer::UnWatch, 0, 0, 0 },
				{ "subscribe",REDIS_CMD_SUBSCRIBE,&ArdbServer::Subscribe, 1, -1, 0 },
				{ "psubscribe",REDIS_CMD_PSUBSCRIBE, &ArdbServer::PSubscribe, 1, -1, 0 },
				{ "unsubscribe",REDIS_CMD_UNSUBSCRIBE, &ArdbServer::UnSubscribe, 0, -1, 0 },
				{ "punsubscribe",REDIS_CMD_PUNSUBSCRIBE, &ArdbServer::PUnSubscribe, 0, -1, 0 },
				{ "publish",REDIS_CMD_PUBLISH, &ArdbServer::Publish, 2, 2, 0 },
				{ "info",REDIS_CMD_INFO, &ArdbServer::Info, 0, 1, 0 },
				{ "save",REDIS_CMD_SAVE, &ArdbServer::Save, 0, 0, 0 },
				{ "bgsave",REDIS_CMD_BGSAVE, &ArdbServer::BGSave, 0, 0, 0 },
				{ "lastsave", REDIS_CMD_LASTSAVE,&ArdbServer::LastSave, 0, 0, 0 },
				{ "slowlog",REDIS_CMD_SLOWLOG, &ArdbServer::SlowLog, 1, 2, 0 },
				{ "dbsize",REDIS_CMD_DBSIZE, &ArdbServer::DBSize, 0, 0, 0 },
				{ "config",REDIS_CMD_CONFIG, &ArdbServer::Config, 1, 3, 0 },
				{ "client",REDIS_CMD_CLIENT, &ArdbServer::Client, 1, 3, 0 },
				{ "flushdb",REDIS_CMD_FLUSHDB, &ArdbServer::FlushDB, 0, 0, 1 },
				{ "flushall",REDIS_CMD_FLUSHALL, &ArdbServer::FlushAll, 0, 0, 1 },
				{ "compactdb", REDIS_CMD_COMPACTDB,&ArdbServer::CompactDB, 0, 0, 1 },
				{ "compactall",REDIS_CMD_COMPACTALL, &ArdbServer::CompactAll, 0, 0, 1 },
				{ "time",REDIS_CMD_TIME, &ArdbServer::Time, 0, 0, 0 },
				{ "echo",REDIS_CMD_ECHO, &ArdbServer::Echo, 1, 1, 0 },
				{ "quit", REDIS_CMD_QUIT,&ArdbServer::Quit, 0, 0, 0 },
				{ "shutdown",REDIS_CMD_SHUTDOWN, &ArdbServer::Shutdown, 0, 1, 0 },
				{ "slaveof", REDIS_CMD_SLAVEOF,&ArdbServer::Slaveof, 2, -1, 0 },
				{ "replconf",REDIS_CMD_REPLCONF, &ArdbServer::ReplConf, 0, -1, 0 },
				{ "sync",EWDIS_CMD_SYNC, &ArdbServer::Sync, 0, 2, 0 },
				{ "arsync",REDIS_CMD_ARSYNC, &ArdbServer::ARSync, 2, -1, 0 },
				{ "select",REDIS_CMD_SELECT, &ArdbServer::Select, 1, 1, 1 },
				{ "append",REDIS_CMD_APPEND, &ArdbServer::Append, 2, 2 },
				{ "get",REDIS_CMD_GET, &ArdbServer::Get, 1, 1, 0 },
				{ "set",REDIS_CMD_SET, &ArdbServer::Set, 2, 7, 1 },
				{ "del",REDIS_CMD_DEL, &ArdbServer::Del, 1, -1, 1 },
				{ "exists",REDIS_CMD_EXISTS, &ArdbServer::Exists, 1, 1, 0 },
				{ "expire",REDIS_CMD_EXPIRE, &ArdbServer::Expire, 2, 2, 1 },
				{ "pexpire",REDIS_CMD_PEXPIRE, &ArdbServer::PExpire, 2, 2, 1 },
				{ "expireat",REDIS_CMD_EXPIREAT, &ArdbServer::Expireat, 2, 2, 1 },
				{ "pexpireat",REDIS_CMD_PEXPIREAT, &ArdbServer::PExpireat, 2, 2, 1 },
				{ "persist",REDIS_CMD_PERSIST, &ArdbServer::Persist, 1, 1, 1 },
				{ "ttl",REDIS_CMD_TTL, &ArdbServer::TTL, 1, 1, 0 },
				{ "pttl", REDIS_CMD_PTTL,&ArdbServer::PTTL, 1, 1, 0 },
				{ "type", REDIS_CMD_TYPE, &ArdbServer::Type, 1, 1, 0 },
				{ "bitcount",REDIS_CMD_BITCOUNT, &ArdbServer::Bitcount, 1, 3, 0 },
				{ "bitop",REDIS_CMD_BITOP, &ArdbServer::Bitop, 3, -1, 1 },
				{ "bitopcount",REDIS_CMD_BITOPCUNT, &ArdbServer::BitopCount, 2, -1, 0 },
				{ "decr", REDIS_CMD_DECR,&ArdbServer::Decr, 1, 1, 1 },
				{ "decrby",REDIS_CMD_DECRBY, &ArdbServer::Decrby, 2, 2, 1 },
				{ "getbit",REDIS_CMD_GETBIT, &ArdbServer::GetBit, 2, 2, 0 },
				{ "getrange",REDIS_CMD_GETRANGE, &ArdbServer::GetRange, 3, 3, 0 },
				{ "getset", REDIS_CMD_GETSET, &ArdbServer::GetSet, 2, 2, 1 },
				{ "incr",REDIS_CMD_INCR, &ArdbServer::Incr, 1, 1, 1 },
				{ "incrby",REDIS_CMD_INCRBY, &ArdbServer::Incrby, 2, 2, 1 },
				{ "incrbyfloat",REDIS_CMD_INCRBYFLOAT, &ArdbServer::IncrbyFloat, 2, 2, 1 },
				{ "mget", REDIS_CMD_MGET,&ArdbServer::MGet, 1, -1, 0 },
				{ "mset", REDIS_CMD_MSET,&ArdbServer::MSet, 2, -1, 1 },
				{ "msetnx",REDIS_CMD_MSETNX, &ArdbServer::MSetNX, 2, -1, 1 },
				{ "psetex",REDIS_CMD_PSETEX, &ArdbServer::MSetNX, 3, 3, 1 },
				{ "setbit",REDIS_CMD_SETBIT, &ArdbServer::SetBit, 3, 3, 1 },
				{ "setex",REDIS_CMD_SETEX, &ArdbServer::SetEX, 3, 3, 1 },
				{ "setnx",REDIS_CMD_SETNX, &ArdbServer::SetNX, 2, 2, 1 },
				{ "setrange", REDIS_CMD_SETEANGE, &ArdbServer::SetRange, 3, 3, 1 },
				{ "strlen", REDIS_CMD_STRLEN, &ArdbServer::Strlen, 1, 1, 0 },
				{ "hdel", REDIS_CMD_HDEL, &ArdbServer::HDel, 2, -1, 1 },
				{ "hexists",REDIS_CMD_HEXISTS, &ArdbServer::HExists, 2, 2, 0 },
				{ "hget",REDIS_CMD_HGET, &ArdbServer::HGet, 2, 2, 0 },
				{ "hgetall",REDIS_CMD_HGETALL, &ArdbServer::HGetAll, 1, 1, 0 },
				{ "hincr",REDIS_CMD_HINCR, &ArdbServer::HIncrby, 3, 3, 1 },
				{ "hmincrby", REDIS_CMD_HMINCRBY, &ArdbServer::HMIncrby, 3, -1, 1 },
				{ "hincrbyfloat",REDIS_CMD_HINCRBYFLOAT, &ArdbServer::HIncrbyFloat, 3, 3, 1 },
				{ "hkeys", REDIS_CMD_HKEYS, &ArdbServer::HKeys, 1, 1, 0 },
				{ "hlen", REDIS_CMD_HLEN, &ArdbServer::HLen, 1, 1, 0 },
				{ "hvals",REDIS_CMD_HVALS, &ArdbServer::HVals, 1, 1, 0 },
				{ "hmget",REDIS_CMD_HMGET, &ArdbServer::HMGet, 2, -1, 0 },
				{ "hset",REDIS_CMD_HSET, &ArdbServer::HSet, 3, 3, 1 },
				{ "hsetnx",REDIS_CMD_HSETNX, &ArdbServer::HSetNX, 3, 3, 1 },
				{ "hmset", REDIS_CMD_HMSET,&ArdbServer::HMSet, 3, -1, 1 },
				{ "scard", REDIS_CMD_SCARD, &ArdbServer::SCard, 1, 1, 0 },
				{ "sadd", REDIS_CMD_SADD, &ArdbServer::SAdd, 2, -1, 1 },
				{ "sdiff",REDIS_CMD_SDIFF, &ArdbServer::SDiff, 2, -1, 0 },
				{ "sdiffcount", REDIS_CMD_SDIFFCOUNT, &ArdbServer::SDiffCount, 2, -1, 0 },
				{ "sdiffstore", REDIS_CMD_SDIFFSTORE,&ArdbServer::SDiffStore, 3, -1, 1 },
				{ "sinter",REDIS_CMD_SINTER, &ArdbServer::SInter, 2, -1, 0 },
				{ "sintercount", REDIS_CMD_SINTERCOUNT, &ArdbServer::SInterCount, 2, -1, 0 },
				{ "sinterstore",REDIS_CMD_SINTERSTORE, &ArdbServer::SInterStore, 3, -1, 1 },
				{ "sismember",REDIS_CMD_SISMEMBER, &ArdbServer::SIsMember, 2, 2, 0 },
				{ "smembers", REDIS_CMD_SMEMBERS,&ArdbServer::SMembers, 1, 1, 0 },
				{ "smove",REDIS_CMD_SMOVE, &ArdbServer::SMove, 3, 3, 1 },
				{ "spop",REDIS_CMD_SPOP, &ArdbServer::SPop, 1, 1, 1 },
				{ "sranmember",REDIS_CMD_SRANMEMEBER, &ArdbServer::SRandMember, 1, 2, 0 },
				{ "srem", REDIS_CMD_SREM, &ArdbServer::SRem, 2, -1, 1 },
				{ "sunion",REDIS_CMD_SUNION, &ArdbServer::SUnion, 2, -1, 0 },
				{ "sunionstore", REDIS_CMD_SUNIONSTORE, &ArdbServer::SUnionStore, 3, -1, 1 },
				{ "sunioncount", REDIS_CMD_SUNIONCOUNT, &ArdbServer::SUnionCount, 2, -1, 0 },
				{ "srange",REDIS_CMD_SRANGE, &ArdbServer::SRange, 3, 4, 0 },
				{ "srevrange",REDIS_CMD_SREVREANGE, &ArdbServer::SRevRange, 3, 4, 0 },
				{ "zadd",REDIS_CMD_ZADD, &ArdbServer::ZAdd, 3, -1, 1 },
				{ "rtazadd",REDIS_CMD_ZADD, &ArdbServer::ZAdd, 3, -1, 1 }, /*Compatible with a modified Redis version*/
				{ "zcard",REDIS_CMD_ZCARD, &ArdbServer::ZCard, 1, 1, 0 },
				{ "zcount", REDIS_CMD_ZCOUNT, &ArdbServer::ZCount, 3, 3, 0 },
				{ "zincrby",REDIS_CMD_ZINCRBY, &ArdbServer::ZIncrby, 3, 3, 1 },
				{ "zrange", REDIS_CMD_ZRANGE, &ArdbServer::ZRange, 3, 4, 0 },
				{ "zrangebyscore", REDIS_CMD_ZRANGEBYSCORE, &ArdbServer::ZRangeByScore, 3, 7, 0 },
				{ "zrank", REDIS_CMD_ZRANK, &ArdbServer::ZRank, 2, 2, 0 },
				{ "zrem",REDIS_CMD_ZREM, &ArdbServer::ZRem, 2, -1, 1 },
				{ "zpop", REDIS_CMD_ZPOP, &ArdbServer::ZPop, 2, 2, 1 },
				{ "zrpop",REDIS_CMD_ZRPOP, &ArdbServer::ZPop, 2, 2, 1 },
				{ "zremrangebyrank", REDIS_CMD_ZREMRANGEBYRANK, &ArdbServer::ZRemRangeByRank, 3, 3, 1 },
				{ "zremrangebyscore", REDIS_CMD_ZREMRANGEBYSCORE, &ArdbServer::ZRemRangeByScore, 3, 3, 1 },
				{ "zrevrange",REDIS_CMD_ZREVRANGE, &ArdbServer::ZRevRange, 3, 4, 0 },
				{ "zrevrangebyscore",REDIS_CMD_ZREVRANGEBYSCORE, &ArdbServer::ZRevRangeByScore, 3, 7, 0 },
				{ "zinterstore",REDIS_CMD_ZINTERSTORE, &ArdbServer::ZInterStore, 3, -1, 1 },
				{ "zunionstore",REDIS_CMD_ZUNIONSTORE, &ArdbServer::ZUnionStore, 3, -1, 1 },
				{ "zrevrank",REDIS_CMD_ZREVRANK, &ArdbServer::ZRevRank, 2, 2, 0 },
				{ "zscore", REDIS_CMD_ZSCORE, &ArdbServer::ZScore, 2, 2, 0 },
				{ "lindex", REDIS_CMD_LINDEX, &ArdbServer::LIndex, 2, 2, 0 },
				{ "linsert",REDIS_CMD_LINSERT, &ArdbServer::LInsert, 4, 4, 1 },
				{ "llen",REDIS_CMD_LLEN, &ArdbServer::LLen, 1, 1, 0 },
				{ "lpop", REDIS_CMD_LPOP, &ArdbServer::LPop, 1, 1, 1 },
				{ "lpush",REDIS_CMD_LPUSH, &ArdbServer::LPush, 2, -1, 1 },
				{ "lpushx",REDIS_CMD_LPUSHX, &ArdbServer::LPushx, 2, 2, 1 },
				{ "lrange",REDIS_CMD_LRANGE, &ArdbServer::LRange, 3, 3, 0 },
				{ "lrem", REDIS_CMD_LREM, &ArdbServer::LRem, 3, 3, 1 },
				{ "lset",REDIS_CMD_LSET, &ArdbServer::LSet, 3, 3, 1 },
				{ "ltrim",REDIS_CMD_LTRIM, &ArdbServer::LTrim, 3, 3, 1 },
				{ "rpop", REDIS_CMD_RPOP, &ArdbServer::RPop, 1, 1, 1 },
				{ "rpush",REDIS_CMD_RPUSH, &ArdbServer::RPush, 2, -1, 1 },
				{ "rpushx", REDIS_CMD_RPUSHX, &ArdbServer::RPushx, 2, 2, 1 },
				{ "rpoplpush", REDIS_CMD_RPOPLPUSH, &ArdbServer::RPopLPush, 2, 2, 1 },
				{ "hclear",REDIS_CMD_HCLEAR, &ArdbServer::HClear, 1, 1, 1 },
				{ "zclear",REDIS_CMD_ZCLEAR, &ArdbServer::ZClear, 1, 1, 1 },
				{ "sclear",REDIS_CMD_SCLEAR, &ArdbServer::SClear, 1, 1, 1 },
				{ "lclear",REDIS_CMD_LCLEAR, &ArdbServer::LClear, 1, 1, 1 },
				{ "move", REDIS_CMD_MOVE, &ArdbServer::Move, 2, 2, 1 },
				{ "rename", REDIS_CMD_RENAME, &ArdbServer::Rename, 2, 2, 1 },
				{ "renamenx",REDIS_CMD_RENAMENX, &ArdbServer::RenameNX, 2, 2, 1 },
				{ "sort",REDIS_CMD_SORT, &ArdbServer::Sort, 1, -1, 2 },
				{ "keys", REDIS_CMD_KEYS, &ArdbServer::Keys, 1, 1, 0 },
				{ "__set__",REDIS_CMD_RAWSET, &ArdbServer::RawSet, 2, 2, 1 },
				{ "__del__", REDIS_CMD_RAWDEL, &ArdbServer::RawDel, 1, 1, 1 },
				{ "tcreate",REDIS_CMD_TCREATE, &ArdbServer::TCreate, 2, -1, 1 },
				{ "tlen",REDIS_CMD_TLEN, &ArdbServer::TLen, 1, 1, 0 },
				{ "tdesc",REDIS_CMD_TDESC, &ArdbServer::TDesc, 1, 1, 0 },
				{ "tinsert",REDIS_CMD_TINSERT, &ArdbServer::TInsert, 6, -1, 1 },
				{ "treplace",REDIS_CMD_TREPLACE, &ArdbServer::TInsert, 6, -1, 1 },
				{ "tget",REDIS_CMD_TGET, &ArdbServer::TGet, 2, -1, 1 },
				{ "tgetall", REDIS_CMD_TGETALL, &ArdbServer::TGetAll, 1, 1, 0 },
				{ "tdel",REDIS_CMD_TDEL, &ArdbServer::TDel, 1, -1, 1 },
				{ "tdelcol",REDIS_CMD_TDELCOL, &ArdbServer::TDelCol, 2, 2, 1 },
				{ "tcreateindex",REDIS_CMD_TCREATEINDEX, &ArdbServer::TCreateIndex, 2, 2, 1 },
				{ "tupdate",REDIS_CMD_TUPDATE, &ArdbServer::TUpdate, 4, -1, 1 },
				{ "eval",REDIS_CMD_TUPDATE, &ArdbServer::Eval, 3, -1, 1 },
				{ "evalsha",REDIS_CMD_TUPDATE, &ArdbServer::EvalSHA, 3, -1, 1 },
				{ "script",REDIS_CMD_TUPDATE, &ArdbServer::Script, 3, -1, 1 },
			};

		uint32 arraylen = arraysize(settingTable);
		for (uint32 i = 0; i < arraylen; i++)
		{
			m_handler_table[settingTable[i].name] = settingTable[i];
		}
	}
	ArdbServer::~ArdbServer()
	{

	}

	static LUAInterpreter* luaCreator(void* data){
		ArdbServer* server = (ArdbServer*)data;
		return new LUAInterpreter(server);
	}

	int ArdbServer::Eval(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 numkey = 0;
		if (!string_touint32(cmd.GetArguments()[1], numkey))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		if(cmd.GetArguments().size() < numkey + 2){
			fill_error_reply(ctx.reply,
								"ERR Wrong number of arguments for Eval");
			return 0;
		}
		SliceArray keys, args;
        for(uint32 i = 2; i < numkey + 2; i++)
        {
          	keys.push_back(cmd.GetArguments()[i]);
        }
        for(uint32 i = numkey + 2; i < cmd.GetArguments().size(); i++)
        {
          	args.push_back(cmd.GetArguments()[i]);
        }
		LUAInterpreter& lua = m_ctx_lua.GetValue(luaCreator, this);
		lua.Eval(cmd.GetArguments()[0],keys, args, false, ctx.reply);
		return 0;
	}

	int ArdbServer::EvalSHA(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 numkey = 0;
		if (!string_touint32(cmd.GetArguments()[1], numkey))
		{
			fill_error_reply(ctx.reply,
							"ERR value is not an integer or out of range");
			return 0;
		}
		if(cmd.GetArguments().size() < numkey + 2){
			fill_error_reply(ctx.reply,
							"ERR Wrong number of arguments for Eval");
			return 0;
		}
		SliceArray keys, args;
		for(uint32 i = 2; i < numkey + 2; i++)
		{
		    keys.push_back(cmd.GetArguments()[i]);
		}
		for(uint32 i = numkey + 2; i < cmd.GetArguments().size(); i++)
		{
		    args.push_back(cmd.GetArguments()[i]);
		}
	    LUAInterpreter& lua = m_ctx_lua.GetValue(luaCreator, this);
		lua.Eval(cmd.GetArguments()[0],keys, args, true, ctx.reply);
		return 0;
	}

	int ArdbServer::Script(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		return 0;
	}

	int ArdbServer::RawSet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->RawSet(cmd.GetArguments()[0], cmd.GetArguments()[1]);
		return 0;
	}
	int ArdbServer::RawDel(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->RawDel(cmd.GetArguments()[0]);
		return 0;
	}
	int ArdbServer::Keys(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		StringSet keys;
		m_db->Keys(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_str_array_reply(ctx.reply, keys);
		return 0;
	}
	int ArdbServer::HClear(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->HClear(ctx.currentDB, cmd.GetArguments()[0]);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::SClear(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->SClear(ctx.currentDB, cmd.GetArguments()[0]);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::ZClear(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->ZClear(ctx.currentDB, cmd.GetArguments()[0]);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::LClear(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->LClear(ctx.currentDB, cmd.GetArguments()[0]);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::Save(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_repli_serv.Save();
		if (ret == 0)
		{
			fill_status_reply(ctx.reply, "OK");
		} else
		{
			fill_error_reply(ctx.reply, "ERR Save error");
		}
		return 0;
	}

	int ArdbServer::LastSave(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_repli_serv.LastSave();
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::BGSave(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_repli_serv.BGSave();
		if (ret == 0)
		{
			fill_status_reply(ctx.reply, "OK");
		} else
		{
			fill_error_reply(ctx.reply, "ERR Save error");
		}
		return 0;
	}

	void ArdbServer::FillInfoResponse(const std::string& section,
			std::string& info)
	{
		if (!strcasecmp(section.c_str(), "all")
				|| !strcasecmp(section.c_str(), "server"))
		{
			info.append("# Server\r\n");
			info.append("ardb_version:").append(ARDB_VERSION).append("\r\n");
			info.append("ardb_home:").append(m_cfg.home).append("\r\n");
			info.append("engine:").append(m_engine.GetName()).append("\r\n");
			if (m_cfg.repl_log_enable)
			{
				info.append("server_key:").append(
						m_repli_serv.GetOpLogs().GetServerKey()).append("\r\n");
			}
			char tmp[256];
			sprintf(tmp, "%"PRId64, m_cfg.listen_port);
			info.append("tcp_port:").append(tmp).append("\r\n");
			sprintf(tmp, "%zu", kServerStat.GetRecvReqCount());
			info.append("recv_req:").append(tmp).append("\r\n");
			sprintf(tmp, "%zu", kServerStat.GetSentReplyCount());
			info.append("sent_reply:").append(tmp).append("\r\n");
		}
		if (!strcasecmp(section.c_str(), "all")
				|| !strcasecmp(section.c_str(), "databases"))
		{
			info.append("# Databases\r\n");
			info.append("data_dir:").append(m_cfg.data_base_path).append(
					"\r\n");
			info.append(m_db->GetEngine()->Stats()).append("\r\n");
		}
		if (!strcasecmp(section.c_str(), "all")
				|| !strcasecmp(section.c_str(), "disk"))
		{
			info.append("# Disk\r\n");
			int64 filesize = file_size(m_cfg.data_base_path);
			char tmp[256];
			sprintf(tmp, "%"PRId64, filesize);
			info.append("db_used_space:").append(tmp).append("\r\n");
		}
		if (!strcasecmp(section.c_str(), "all")
				|| !strcasecmp(section.c_str(), "oplogs"))
		{
			info.append("# Oplogs\r\n");
			if (m_cfg.repl_log_enable)
			{
				char tmp[1024];
				sprintf(tmp, "%u", m_repli_serv.GetOpLogs().MemCacheSize());
				info.append("oplogs_cache_size:").append(tmp).append("\r\n");
				info.append("oplogs_cache_seq: [");
				sprintf(tmp, "%"PRIu64, m_repli_serv.GetOpLogs().GetMinSeq());
				info.append(tmp).append("-");
				sprintf(tmp, "%"PRIu64, m_repli_serv.GetOpLogs().GetMaxSeq());
				info.append(tmp).append("]\r\n");
			} else
			{
				info.append("repl-log-enable: false\r\n");
			}
		}

		if (!strcasecmp(section.c_str(), "all")
				|| !strcasecmp(section.c_str(), "memory"))
		{
			info.append("# Memory\r\n");
		}
	}

	int ArdbServer::Info(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string info;
		std::string section = "all";
		if (cmd.GetArguments().size() == 1)
		{
			section = cmd.GetArguments()[0];
		}
		FillInfoResponse(section, info);
		fill_str_reply(ctx.reply, info);
		return 0;
	}

	int ArdbServer::DBSize(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		fill_int_reply(ctx.reply, file_size(m_cfg.data_base_path));
		return 0;
	}

	int ArdbServer::Client(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string subcmd = string_tolower(cmd.GetArguments()[0]);
		if (subcmd == "kill")
		{
			if (cmd.GetArguments().size() != 2)
			{
				fill_error_reply(ctx.reply,
						"ERR Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name | STAT on/off)");
				return 0;
			}
			Channel* conn = m_clients_holder.GetConn(cmd.GetArguments()[1]);
			if (NULL == conn)
			{
				fill_error_reply(ctx.reply, "ERR No such client");
				return 0;
			}
			fill_status_reply(ctx.reply, "OK");
			if (conn == ctx.conn)
			{
				return -1;
			} else
			{
				conn->Close();
			}
		} else if (subcmd == "setname")
		{
			if (cmd.GetArguments().size() != 2)
			{
				fill_error_reply(ctx.reply,
						"ERR Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name | STAT on/off)");
				return 0;
			}
			m_clients_holder.SetName(ctx.conn, cmd.GetArguments()[1]);
			fill_status_reply(ctx.reply, "OK");
			return 0;
		} else if (subcmd == "getname")
		{
			if (cmd.GetArguments().size() != 1)
			{
				fill_error_reply(ctx.reply,
						"ERR Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name | STAT on/off)");
				return 0;
			}
			fill_str_reply(ctx.reply, m_clients_holder.GetName(ctx.conn));
		} else if (subcmd == "list")
		{
			if (cmd.GetArguments().size() != 1)
			{
				fill_error_reply(ctx.reply,
						"ERR Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name | STAT on/off)");
				return 0;
			}
			m_clients_holder.List(ctx.reply);
		} else if (subcmd == "stat")
		{
			if (cmd.GetArguments().size() != 2)
			{
				fill_error_reply(ctx.reply,
						"ERR Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name | STAT on/off)");
				return 0;
			}
			if (!strcasecmp(cmd.GetArguments()[1].c_str(), "on"))
			{
				m_clients_holder.SetStatEnable(true);
			} else if (!strcasecmp(cmd.GetArguments()[1].c_str(), "off"))
			{
				m_clients_holder.SetStatEnable(false);
			} else
			{
				fill_error_reply(ctx.reply,
						"ERR Syntax error, try CLIENT (LIST | KILL ip:port | GETNAME | SETNAME connection-name | STAT on/off)");
				return 0;
			}
			fill_status_reply(ctx.reply, "OK");
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR CLIENT subcommand must be one of LIST, GETNAME, SETNAME, KILL, STAT");
		}
		return 0;
	}

	int ArdbServer::SlowLog(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string subcmd = string_tolower(cmd.GetArguments()[0]);
		if (subcmd == "len")
		{
			fill_int_reply(ctx.reply, m_slowlog_handler.Size());
		} else if (subcmd == "reset")
		{
			fill_status_reply(ctx.reply, "OK");
		} else if (subcmd == "get")
		{
			if (cmd.GetArguments().size() != 2)
			{
				fill_error_reply(ctx.reply,
						"ERR Wrong number of arguments for SLOWLOG GET");
			}
			uint32 len = 0;
			if (!string_touint32(cmd.GetArguments()[1], len))
			{
				fill_error_reply(ctx.reply,
						"ERR value is not an integer or out of range.");
				return 0;
			}
			m_slowlog_handler.GetSlowlog(len, ctx.reply);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR SLOWLOG subcommand must be one of GET, LEN, RESET");
		}
		return 0;
	}

	int ArdbServer::Config(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string arg0 = string_tolower(cmd.GetArguments()[0]);
		if (arg0 != "get" && arg0 != "set" && arg0 != "resetstat")
		{
			fill_error_reply(ctx.reply,
					"ERR CONFIG subcommand must be one of GET, SET, RESETSTAT");
			return 0;
		}
		if (arg0 == "resetstat")
		{
			if (cmd.GetArguments().size() != 1)
			{
				fill_error_reply(ctx.reply,
						"ERR Wrong number of arguments for CONFIG RESETSTAT");
				return 0;
			}
		} else if (arg0 == "get")
		{
			if (cmd.GetArguments().size() != 2)
			{
				fill_error_reply(ctx.reply,
						"ERR Wrong number of arguments for CONFIG GET");
				return 0;
			}
			ctx.reply.type = REDIS_REPLY_ARRAY;
			Properties::iterator it = m_cfg_props.begin();
			while (it != m_cfg_props.end())
			{
				if (fnmatch(cmd.GetArguments()[1].c_str(), it->first.c_str(), 0)
						== 0)
				{
					ctx.reply.elements.push_back(RedisReply(it->first));
					ctx.reply.elements.push_back(RedisReply(it->second));
				}
				it++;
			}
		} else if (arg0 == "set")
		{
			if (cmd.GetArguments().size() != 3)
			{
				fill_error_reply(ctx.reply,
						"RR Wrong number of arguments for CONFIG SET");
				return 0;
			}
			m_cfg_props[cmd.GetArguments()[1]] = cmd.GetArguments()[2];
			ParseConfig(m_cfg_props, m_cfg);
			fill_status_reply(ctx.reply, "OK");
		}
		return 0;
	}

	int ArdbServer::Time(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint64 micros = get_current_epoch_micros();
		ValueArray vs;
		vs.push_back(ValueObject((int64) micros / 1000000));
		vs.push_back(ValueObject((int64) micros % 1000000));
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::FlushDB(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->FlushDB(ctx.currentDB);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::FlushAll(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->FlushAll();
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::Rename(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->Rename(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		if (ret < 0)
		{
			fill_error_reply(ctx.reply, "ERR no such key");
		} else
		{
			fill_status_reply(ctx.reply, "OK");
		}
		return 0;
	}

	int ArdbServer::Sort(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vs;
		std::string key = cmd.GetArguments()[0];
		cmd.GetArguments().pop_front();
		int ret = m_db->Sort(ctx.currentDB, key, cmd.GetArguments(), vs);
		if (ret < 0)
		{
			fill_error_reply(ctx.reply,
					"Invalid SORT command or invalid state for SORT.");
		} else
		{
			fill_array_reply(ctx.reply, vs);
		}
		cmd.GetArguments().push_front(key);
		return 0;
	}

	int ArdbServer::RenameNX(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->RenameNX(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_int_reply(ctx.reply, ret < 0 ? 0 : 1);
		return 0;
	}

	int ArdbServer::Move(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		DBID dst = 0;
		if (!string_touint32(cmd.GetArguments()[1], dst))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->Move(ctx.currentDB, cmd.GetArguments()[0], dst);
		fill_int_reply(ctx.reply, ret < 0 ? 0 : 1);
		return 0;
	}

	int ArdbServer::Type(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->Type(ctx.currentDB, cmd.GetArguments()[0]);
		switch (ret)
		{
			case SET_ELEMENT:
			{
				fill_status_reply(ctx.reply, "set");
				break;
			}
			case LIST_META:
			{
				fill_status_reply(ctx.reply, "list");
				break;
			}
			case ZSET_ELEMENT_SCORE:
			{
				fill_status_reply(ctx.reply, "zset");
				break;
			}
			case HASH_FIELD:
			{
				fill_status_reply(ctx.reply, "hash");
				break;
			}
			case KV:
			{
				fill_status_reply(ctx.reply, "string");
				break;
			}
			case TABLE_META:
			{
				fill_status_reply(ctx.reply, "table");
				break;
			}
			case BITSET_META:
			{
				fill_status_reply(ctx.reply, "bitset");
				break;
			}
			default:
			{
				fill_status_reply(ctx.reply, "none");
				break;
			}
		}
		return 0;
	}

	int ArdbServer::Persist(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->Persist(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret == 0 ? 1 : 0);
		return 0;
	}

	int ArdbServer::PExpire(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 v = 0;
		if (!check_uint32_arg(ctx.reply, cmd.GetArguments()[1], v))
		{
			return 0;
		}
		int ret = m_db->Pexpire(ctx.currentDB, cmd.GetArguments()[0], v);
		fill_int_reply(ctx.reply, ret >= 0 ? 1 : 0);
		return 0;
	}
	int ArdbServer::PExpireat(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint64 v = 0;
		if (!check_uint64_arg(ctx.reply, cmd.GetArguments()[1], v))
		{
			return 0;
		}
		int ret = m_db->Pexpireat(ctx.currentDB, cmd.GetArguments()[0], v);
		fill_int_reply(ctx.reply, ret >= 0 ? 1 : 0);
		return 0;
	}
	int ArdbServer::PTTL(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64 ret = m_db->PTTL(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret >= 0 ? ret : -1);
		return 0;
	}
	int ArdbServer::TTL(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64 ret = m_db->TTL(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret >= 0 ? ret : -1);
		return 0;
	}

	int ArdbServer::Expire(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 v = 0;
		if (!check_uint32_arg(ctx.reply, cmd.GetArguments()[1], v))
		{
			return 0;
		}
		int ret = m_db->Expire(ctx.currentDB, cmd.GetArguments()[0], v);
		fill_int_reply(ctx.reply, ret >= 0 ? 1 : 0);
		return 0;
	}

	int ArdbServer::Expireat(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 v = 0;
		if (!check_uint32_arg(ctx.reply, cmd.GetArguments()[1], v))
		{
			return 0;
		}
		int ret = m_db->Expireat(ctx.currentDB, cmd.GetArguments()[0], v);
		fill_int_reply(ctx.reply, ret >= 0 ? 1 : 0);
		return 0;
	}

	int ArdbServer::Exists(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		bool ret = m_db->Exists(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret ? 1 : 0);
		return 0;
	}

	int ArdbServer::Del(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray array;
		ArgumentArray::iterator it = cmd.GetArguments().begin();
		while (it != cmd.GetArguments().end())
		{
			array.push_back(*it);
			it++;
		}
		int ret = m_db->Del(ctx.currentDB, array);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::Set(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		const std::string& key = cmd.GetArguments()[0];
		const std::string& value = cmd.GetArguments()[1];
		int ret = 0;
		if (cmd.GetArguments().size() == 2)
		{
			ret = m_db->Set(ctx.currentDB, key, value);
		} else
		{
			uint32 i = 0;
			uint64 px = 0, ex = 0;
			for (i = 2; i < cmd.GetArguments().size(); i++)
			{
				std::string tmp = string_tolower(cmd.GetArguments()[i]);
				if (tmp == "ex" || tmp == "px")
				{
					int64 iv;
					if (!raw_toint64(cmd.GetArguments()[i + 1].c_str(),
							cmd.GetArguments()[i + 1].size(), iv) || iv < 0)
					{
						fill_error_reply(ctx.reply,
								"ERR value is not an integer or out of range");
						return 0;
					}
					if (tmp == "px")
					{
						px = iv;
					} else
					{
						ex = iv;
					}
					i++;
				} else
				{
					break;
				}
			}
			bool hasnx = false, hasxx = false;
			bool syntaxerror = false;
			if (i < cmd.GetArguments().size() - 1)
			{
				syntaxerror = true;
			}
			if (i == cmd.GetArguments().size() - 1)
			{
				std::string cmp = string_tolower(cmd.GetArguments()[i]);
				if (cmp != "nx" && cmp != "xx")
				{
					syntaxerror = true;
				} else
				{
					hasnx = cmp == "nx";
					hasxx = cmp == "xx";
				}
			}
			if (syntaxerror)
			{
				fill_error_reply(ctx.reply, "ERR syntax error");
				return 0;
			}
			int nxx = 0;
			if (hasnx)
			{
				nxx = -1;
			}
			if (hasxx)
			{
				nxx = 1;
			}
			ret = m_db->Set(ctx.currentDB, key, value, ex, px, nxx);
		}
		if (0 == ret)
		{
			fill_status_reply(ctx.reply, "OK");
		} else
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		}
		return 0;
	}

	int ArdbServer::Get(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		const std::string& key = cmd.GetArguments()[0];
		std::string value;
		if (0 == m_db->Get(ctx.currentDB, key, &value))
		{
			fill_str_reply(ctx.reply, value);
			//ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		}
		return 0;
	}

	int ArdbServer::SetEX(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 secs;
		if (!string_touint32(cmd.GetArguments()[1], secs))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		m_db->SetEx(ctx.currentDB, cmd.GetArguments()[0], cmd.GetArguments()[2],
				secs);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::SetNX(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->SetNX(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}
	int ArdbServer::SetRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int32 offset;
		if (!string_toint32(cmd.GetArguments()[1], offset))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->SetRange(ctx.currentDB, cmd.GetArguments()[0], offset,
				cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}
	int ArdbServer::Strlen(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->Strlen(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SetBit(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int32 offset;
		if (!string_toint32(cmd.GetArguments()[1], offset))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		if (cmd.GetArguments()[2] != "1" && cmd.GetArguments()[2] != "0")
		{
			fill_error_reply(ctx.reply,
					"ERR bit is not an integer or out of range");
			return 0;
		}
		uint8 bit = cmd.GetArguments()[2] != "0";
		int ret = m_db->SetBit(ctx.currentDB, cmd.GetArguments()[0], offset,
				bit);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::PSetEX(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 mills;
		if (!string_touint32(cmd.GetArguments()[1], mills))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		m_db->PSetEx(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[2], mills);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::MSetNX(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if (cmd.GetArguments().size() % 2 != 0)
		{
			fill_error_reply(ctx.reply,
					"ERR wrong number of arguments for MSETNX");
			return 0;
		}
		SliceArray keys;
		SliceArray vals;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i += 2)
		{
			keys.push_back(cmd.GetArguments()[i]);
			vals.push_back(cmd.GetArguments()[i + 1]);
		}
		int count = m_db->MSetNX(ctx.currentDB, keys, vals);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::MSet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if (cmd.GetArguments().size() % 2 != 0)
		{
			fill_error_reply(ctx.reply,
					"ERR wrong number of arguments for MSET");
			return 0;
		}
		SliceArray keys;
		SliceArray vals;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i += 2)
		{
			keys.push_back(cmd.GetArguments()[i]);
			vals.push_back(cmd.GetArguments()[i + 1]);
		}
		m_db->MSet(ctx.currentDB, keys, vals);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::MGet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueArray res;
		m_db->MGet(ctx.currentDB, keys, res);
		fill_array_reply(ctx.reply, res);
		return 0;
	}

	int ArdbServer::IncrbyFloat(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		double increment, val;
		if (!string_todouble(cmd.GetArguments()[1], increment))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not a float or out of range");
			return 0;
		}
		int ret = m_db->IncrbyFloat(ctx.currentDB, cmd.GetArguments()[0],
				increment, val);
		if (ret == 0)
		{
			fill_double_reply(ctx.reply, val);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR value is not a float or out of range");
		}
		return 0;
	}

	int ArdbServer::Incrby(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64 increment, val;
		if (!string_toint64(cmd.GetArguments()[1], increment))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->Incrby(ctx.currentDB, cmd.GetArguments()[0], increment,
				val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::Incr(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64_t val;
		int ret = m_db->Incr(ctx.currentDB, cmd.GetArguments()[0], val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::GetSet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string v;
		int ret = m_db->GetSet(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], v);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_str_reply(ctx.reply, v);
		}
		return 0;
	}

	int ArdbServer::GetRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int32 start, end;
		if (!string_toint32(cmd.GetArguments()[1], start)
				|| !string_toint32(cmd.GetArguments()[2], end))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		std::string v;
		m_db->GetRange(ctx.currentDB, cmd.GetArguments()[0], start, end, v);
		fill_str_reply(ctx.reply, v);
		return 0;
	}

	int ArdbServer::GetBit(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int32 offset;
		if (!string_toint32(cmd.GetArguments()[1], offset))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->GetBit(ctx.currentDB, cmd.GetArguments()[0], offset);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::Decrby(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64 decrement, val;
		if (!string_toint64(cmd.GetArguments()[1], decrement))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->Decrby(ctx.currentDB, cmd.GetArguments()[0], decrement,
				val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::Decr(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64_t val;
		int ret = m_db->Decr(ctx.currentDB, cmd.GetArguments()[0], val);
		if (ret == 0)
		{
			fill_int_reply(ctx.reply, val);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
		}
		return 0;
	}

	int ArdbServer::BitopCount(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		int64 ret = m_db->BitOPCount(ctx.currentDB, cmd.GetArguments()[0],
				keys);
		if (ret < 0)
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
		} else
		{
			fill_int_reply(ctx.reply, ret);
		}
		return 0;
	}

	int ArdbServer::Bitop(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 2; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		int ret = m_db->BitOP(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], keys);
		if (ret < 0)
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
		} else
		{
			fill_int_reply(ctx.reply, ret);
		}
		return 0;
	}

	int ArdbServer::Bitcount(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if (cmd.GetArguments().size() == 2)
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		int count = 0;
		if (cmd.GetArguments().size() == 1)
		{
			count = m_db->BitCount(ctx.currentDB, cmd.GetArguments()[0], 0, -1);
		} else
		{
			int32 start, end;
			if (!string_toint32(cmd.GetArguments()[1], start)
					|| !string_toint32(cmd.GetArguments()[2], end))
			{
				fill_error_reply(ctx.reply,
						"ERR value is not an integer or out of range");
				return 0;
			}
			count = m_db->BitCount(ctx.currentDB, cmd.GetArguments()[0], start,
					end);
		}
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::Append(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		const std::string& key = cmd.GetArguments()[0];
		const std::string& value = cmd.GetArguments()[1];
		int ret = m_db->Append(ctx.currentDB, key, value);
		if (ret > 0)
		{
			fill_int_reply(ctx.reply, ret);
		} else
		{
			fill_error_reply(ctx.reply, "ERR failed to append key:%s",
					key.c_str());
		}
		return 0;
	}

	int ArdbServer::Ping(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		fill_status_reply(ctx.reply, "PONG");
		return 0;
	}
	int ArdbServer::Echo(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		fill_str_reply(ctx.reply, cmd.GetArguments()[0]);
		return 0;
	}
	int ArdbServer::Select(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if (!string_touint32(cmd.GetArguments()[0], ctx.currentDB)
				|| ctx.currentDB > 0xFFFFFF)
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		m_clients_holder.ChangeCurrentDB(ctx.conn, ctx.currentDB);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::Quit(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		fill_status_reply(ctx.reply, "OK");
		return -1;
	}

	int ArdbServer::Shutdown(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_service->Stop();
		return -1;
	}

	int ArdbServer::CompactDB(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->CompactDB(ctx.currentDB);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::CompactAll(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->CompactAll();
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::Slaveof(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		const std::string& host = cmd.GetArguments()[0];
		uint32 port = 0;
		if (!string_touint32(cmd.GetArguments()[1], port))
		{
			if (!strcasecmp(cmd.GetArguments()[0].c_str(), "no")
					&& !strcasecmp(cmd.GetArguments()[1].c_str(), "one"))
			{
				fill_status_reply(ctx.reply, "OK");
				m_slave_client.Stop();
				return 0;
			}
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		DBIDSet idset;
		if (cmd.GetArguments().size() > 2)
		{
			DBID syncdb;
			if (!string_touint32(cmd.GetArguments()[2], syncdb)
					|| syncdb > 0xFFFFFF)
			{
				fill_error_reply(ctx.reply,
						"ERR value is not an integer or out of range");
				return 0;
			}
			idset.insert(syncdb);
		}
		m_slave_client.SetSyncDBs(idset);
		m_slave_client.ConnectMaster(host, port);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::ReplConf(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		DEBUG_LOG("%s %s", cmd.GetArguments()[0].c_str(),
				cmd.GetArguments()[1].c_str());
		if (cmd.GetArguments().size() % 2 != 0)
		{
			fill_error_reply(ctx.reply,
					"ERR wrong number of arguments for ReplConf");
			return 0;
		}
		for (uint32 i = 0; i < cmd.GetArguments().size(); i += 2)
		{
			if (cmd.GetArguments()[i] == "listening-port")
			{
				uint32 port = 0;
				string_touint32(cmd.GetArguments()[i + 1], port);
				Address* addr =
						const_cast<Address*>(ctx.conn->GetRemoteAddress());
				if (InstanceOf<SocketHostAddress>(addr).OK)
				{
					SocketHostAddress* tmp = (SocketHostAddress*) addr;
					const SocketHostAddress& master_addr =
							m_slave_client.GetMasterAddress();
					if (master_addr.GetHost() == tmp->GetHost()
							&& master_addr.GetPort() == port)
					{
						m_repli_serv.MarkMasterSlave(ctx.conn);
					}
				}
			}
		}
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::Sync(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_repli_serv.ServSlaveClient(ctx.conn);
		return 0;
	}

	int ArdbServer::ARSync(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string& serverKey = cmd.GetArguments()[0];
		uint64 seq;
		if (!string_touint64(cmd.GetArguments()[1], seq))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		DBIDSet syncdbs;
		if (cmd.GetArguments().size() > 2)
		{
			for (uint32 i = 2; i < cmd.GetArguments().size(); i++)
			{
				DBID syncdb;
				if (!string_touint32(cmd.GetArguments()[i], syncdb)
						|| syncdb > 0xFFFFFF)
				{
					fill_error_reply(ctx.reply,
							"ERR value is not an integer or out of range");
					return 0;
				}
				syncdbs.insert(syncdb);
			}
		}
		if (m_cfg.repl_log_enable)
		{
			m_repli_serv.ServARSlaveClient(ctx.conn, serverKey, seq, syncdbs);
		} else
		{
			fill_error_reply(ctx.reply,
					"ERR Ardb instance's replication log not enabled");
			return -1;
		}
		return 0;
	}

	int ArdbServer::HMSet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if ((cmd.GetArguments().size() - 1) % 2 != 0)
		{
			fill_error_reply(ctx.reply,
					"ERR wrong number of arguments for HMSet");
			return 0;
		}
		SliceArray fs;
		SliceArray vals;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i += 2)
		{
			fs.push_back(cmd.GetArguments()[i]);
			vals.push_back(cmd.GetArguments()[i + 1]);
		}
		m_db->HMSet(ctx.currentDB, cmd.GetArguments()[0], fs, vals);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::HSet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->HSet(ctx.currentDB, cmd.GetArguments()[0], cmd.GetArguments()[1],
				cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, 1);
		return 0;
	}
	int ArdbServer::HSetNX(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->HSetNX(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}
	int ArdbServer::HVals(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		StringArray keys;
		m_db->HVals(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_str_array_reply(ctx.reply, keys);
		return 0;
	}

	int ArdbServer::HMGet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vals;
		SliceArray fs;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			fs.push_back(cmd.GetArguments()[i]);
		}
		m_db->HMGet(ctx.currentDB, cmd.GetArguments()[0], fs, vals);
		fill_array_reply(ctx.reply, vals);
		return 0;
	}

	int ArdbServer::HLen(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int len = m_db->HLen(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, len);
		return 0;
	}

	int ArdbServer::HKeys(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		StringArray keys;
		m_db->HKeys(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_str_array_reply(ctx.reply, keys);
		return 0;
	}

	int ArdbServer::HIncrbyFloat(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		double increment, val = 0;
		if (!string_todouble(cmd.GetArguments()[2], increment))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not a float or out of range");
			return 0;
		}
		m_db->HIncrbyFloat(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], increment, val);
		fill_double_reply(ctx.reply, val);
		return 0;
	}

	int ArdbServer::HMIncrby(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if ((cmd.GetArguments().size() - 1) % 2 != 0)
		{
			fill_error_reply(ctx.reply,
					"ERR wrong number of arguments for HMIncrby");
			return 0;
		}
		SliceArray fs;
		Int64Array incs;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i += 2)
		{
			fs.push_back(cmd.GetArguments()[i]);
			int64 v = 0;
			if (!string_toint64(cmd.GetArguments()[i + 1], v))
			{
				fill_error_reply(ctx.reply,
						"ERR value is not a integer or out of range");
				return 0;
			}
			incs.push_back(v);
		}
		Int64Array vs;
		m_db->HMIncrby(ctx.currentDB, cmd.GetArguments()[0], fs, incs, vs);
		fill_int_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::HIncrby(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int64 increment, val = 0;
		if (!string_toint64(cmd.GetArguments()[2], increment))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		m_db->HIncrby(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], increment, val);
		fill_int_reply(ctx.reply, val);
		return 0;
	}

	int ArdbServer::HGetAll(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		StringArray fields;
		ValueArray results;
		m_db->HGetAll(ctx.currentDB, cmd.GetArguments()[0], fields, results);
		ctx.reply.type = REDIS_REPLY_ARRAY;
		for (uint32 i = 0; i < fields.size(); i++)
		{
			RedisReply reply1, reply2;
			fill_str_reply(reply1, fields[i]);
			std::string str;
			fill_str_reply(reply2, results[i].ToString(str));
			ctx.reply.elements.push_back(reply1);
			ctx.reply.elements.push_back(reply2);
		}
		return 0;
	}

	int ArdbServer::HGet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string v;
		int ret = m_db->HGet(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], &v);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_str_reply(ctx.reply, v);
		}
		return 0;
	}

	int ArdbServer::HExists(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->HExists(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::HDel(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray fields;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			fields.push_back(cmd.GetArguments()[i]);
		}
		int ret = m_db->HDel(ctx.currentDB, cmd.GetArguments()[0], fields);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

//========================Set CMDs==============================
	int ArdbServer::SAdd(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray values;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			values.push_back(cmd.GetArguments()[i]);
		}
		int count = m_db->SAdd(ctx.currentDB, cmd.GetArguments()[0], values);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::SCard(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->SCard(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret > 0 ? ret : 0);
		return 0;
	}

	int ArdbServer::SDiff(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueSet vs;
		m_db->SDiff(ctx.currentDB, keys, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::SDiffStore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueSet vs;
		int ret = m_db->SDiffStore(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SInter(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueSet vs;
		m_db->SInter(ctx.currentDB, keys, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::SInterStore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueSet vs;
		int ret = m_db->SInterStore(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SIsMember(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->SIsMember(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SMembers(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vs;
		m_db->SMembers(ctx.currentDB, cmd.GetArguments()[0], vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::SMove(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->SMove(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SPop(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string res;
		m_db->SPop(ctx.currentDB, cmd.GetArguments()[0], res);
		fill_str_reply(ctx.reply, res);
		return 0;
	}

	int ArdbServer::SRandMember(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vs;
		int32 count = 1;
		if (cmd.GetArguments().size() > 1)
		{
			if (!string_toint32(cmd.GetArguments()[1], count))
			{
				fill_error_reply(ctx.reply,
						"ERR value is not an integer or out of range");
				return 0;
			}
		}
		m_db->SRandMember(ctx.currentDB, cmd.GetArguments()[0], vs, count);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::SRem(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueSet vs;
		int ret = m_db->SRem(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SUnion(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		ValueSet vs;
		m_db->SUnion(ctx.currentDB, keys, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::SUnionStore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		int ret = m_db->SUnionStore(ctx.currentDB, cmd.GetArguments()[0], keys);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::SUnionCount(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		uint32 count = 0;
		m_db->SUnionCount(ctx.currentDB, keys, count);
		fill_int_reply(ctx.reply, count);
		return 0;
	}
	int ArdbServer::SInterCount(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		uint32 count = 0;
		m_db->SInterCount(ctx.currentDB, keys, count);
		fill_int_reply(ctx.reply, count);
		return 0;
	}
	int ArdbServer::SDiffCount(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		for (uint32 i = 0; i < cmd.GetArguments().size(); i++)
		{
			keys.push_back(cmd.GetArguments()[i]);
		}
		uint32 count = 0;
		m_db->SDiffCount(ctx.currentDB, keys, count);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::SRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vs;
		int32 count = -1;
		if (!string_toint32(cmd.GetArguments()[2], count))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		bool with_first = true;
		if (cmd.GetArguments().size() == 4)
		{
			if (!strcasecmp(cmd.GetArguments()[3].c_str(), "withoutstart"))
			{
				with_first = false;
			} else
			{
				fill_error_reply(ctx.reply,
						"ERR SRANGE last argument must be WITHOUTSTART");
				return 0;
			}
		}
		m_db->SRange(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], count, with_first, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}
	int ArdbServer::SRevRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vs;
		int32 count = -1;
		if (!string_toint32(cmd.GetArguments()[2], count))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		bool with_first = true;
		if (cmd.GetArguments().size() == 4)
		{
			if (!strcasecmp(cmd.GetArguments()[3].c_str(), "withoutstart"))
			{
				with_first = false;
			} else
			{
				fill_error_reply(ctx.reply,
						"ERR SREVRANGE last argument must be WITHOUTSTART");
				return 0;
			}
		}
		m_db->SRevRange(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], count, with_first, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

//===========================Sorted Sets cmds==============================
	int ArdbServer::ZAdd(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		if ((cmd.GetArguments().size() - 1) % 2 != 0)
		{
			fill_error_reply(ctx.reply,
					"ERR wrong number of arguments for ZAdd");
			return 0;
		}
		DoubleArray scores;
		SliceArray svs;
		bool with_limit = false;
		uint32 limit = 0;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i += 2)
		{
			/*
			 * "rta_with_max is for passing compliance on a modified redis version"
			 */
			if (!strcasecmp(cmd.GetArguments()[i].c_str(), "limit")
					|| !strcasecmp(cmd.GetArguments()[i].c_str(),
							"rta_with_max"))
			{
				if (!string_touint32(cmd.GetArguments()[i + 1], limit))
				{
					fill_error_reply(ctx.reply,
							"ERR value is not an integer or out of range");
					return 0;
				}
				with_limit = true;
				break;
			}
			double score;
			if (!string_todouble(cmd.GetArguments()[i], score))
			{
				fill_error_reply(ctx.reply,
						"ERR value is not a float or out of range");
				return 0;
			}
			scores.push_back(score);
			svs.push_back(cmd.GetArguments()[i + 1]);
		}
		if (with_limit)
		{
			ValueArray vs;
			m_db->ZAddLimit(ctx.currentDB, cmd.GetArguments()[0], scores, svs,
					limit, vs);
			fill_array_reply(ctx.reply, vs);
		} else
		{
			int count = m_db->ZAdd(ctx.currentDB, cmd.GetArguments()[0], scores,
					svs);
			fill_int_reply(ctx.reply, count);
		}
		return 0;
	}

	int ArdbServer::ZCard(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->ZCard(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::ZCount(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->ZCount(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::ZIncrby(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		double increment, value;
		if (!string_todouble(cmd.GetArguments()[1], increment))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not a float or out of range");
			return 0;
		}
		m_db->ZIncrby(ctx.currentDB, cmd.GetArguments()[0], increment,
				cmd.GetArguments()[2], value);
		return 0;
	}

	int ArdbServer::ZRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		bool withscores = false;
		if (cmd.GetArguments().size() == 4)
		{
			if (strcasecmp(cmd.GetArguments()[3].c_str(), "withscores") != 0)
			{
				fill_error_reply(ctx.reply, "ERR syntax error");
				return 0;
			}
			withscores = true;
		}
		int start, stop;
		if (!string_toint32(cmd.GetArguments()[1], start)
				|| !string_toint32(cmd.GetArguments()[2], stop))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		QueryOptions options;
		options.withscores = withscores;
		ValueArray vs;
		m_db->ZRange(ctx.currentDB, cmd.GetArguments()[0], start, stop, vs,
				options);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	static bool process_query_options(ArgumentArray& cmd, uint32 idx,
			QueryOptions& options)
	{
		if (cmd.size() > idx)
		{
			std::string optionstr = string_tolower(cmd[3]);
			if (optionstr != "withscores" && optionstr != "limit")
			{
				return false;
			}
			if (optionstr == "withscores")
			{
				options.withscores = true;
				return process_query_options(cmd, idx + 1, options);
			} else
			{
				if (cmd.size() != idx + 3)
				{
					return false;
				}
				if (!string_toint32(cmd[idx + 1], options.limit_offset)
						|| !string_toint32(cmd[idx + 2], options.limit_count))
				{
					return false;
				}
				options.withlimit = true;
				return true;
			}
		} else
		{
			return false;
		}
	}

	int ArdbServer::ZRangeByScore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		QueryOptions options;
		if (cmd.GetArguments().size() >= 4)
		{
			bool ret = process_query_options(cmd.GetArguments(), 3, options);
			if (!ret)
			{
				fill_error_reply(ctx.reply, "ERR syntax error");
				return 0;
			}
		}

		ValueArray vs;
		m_db->ZRangeByScore(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2], vs, options);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::ZRank(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->ZRank(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_int_reply(ctx.reply, ret);
		}
		return 0;
	}

	int ArdbServer::ZPop(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		uint32 num;
		if (!string_touint32(cmd.GetArguments()[1], num))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		ValueArray vs;
		bool reverse = (strcasecmp(cmd.GetCommand().c_str(), "zrpop") == 0);
		m_db->ZPop(ctx.currentDB, cmd.GetArguments()[0], reverse, num, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::ZRem(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int count = 0;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			count += m_db->ZRem(ctx.currentDB, cmd.GetArguments()[0],
					cmd.GetArguments()[i]);
		}
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::ZRemRangeByRank(ArdbConnContext& ctx,
			RedisCommandFrame& cmd)
	{
		int start, stop;
		if (!string_toint32(cmd.GetArguments()[1], start)
				|| !string_toint32(cmd.GetArguments()[2], stop))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int count = m_db->ZRemRangeByRank(ctx.currentDB, cmd.GetArguments()[0],
				start, stop);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::ZRemRangeByScore(ArdbConnContext& ctx,
			RedisCommandFrame& cmd)
	{
		int count = m_db->ZRemRangeByScore(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::ZRevRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		bool withscores = false;
		if (cmd.GetArguments().size() == 4)
		{
			if (string_tolower(cmd.GetArguments()[3]) != "WITHSCORES")
			{
				fill_error_reply(ctx.reply, "ERR syntax error");
				return 0;
			}
			withscores = true;
		}
		int start, stop;
		if (!string_toint32(cmd.GetArguments()[1], start)
				|| !string_toint32(cmd.GetArguments()[2], stop))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		QueryOptions options;
		options.withscores = withscores;
		ValueArray vs;
		m_db->ZRevRange(ctx.currentDB, cmd.GetArguments()[0], start, stop, vs,
				options);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::ZRevRangeByScore(ArdbConnContext& ctx,
			RedisCommandFrame& cmd)
	{
		QueryOptions options;
		if (cmd.GetArguments().size() >= 4)
		{
			bool ret = process_query_options(cmd.GetArguments(), 3, options);
			if (!ret)
			{
				fill_error_reply(ctx.reply, "ERR syntax error");
				return 0;
			}
		}

		ValueArray vs;
		m_db->ZRevRangeByScore(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2], vs, options);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::ZRevRank(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->ZRevRank(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_int_reply(ctx.reply, ret);
		}
		return 0;
	}

	static bool process_zstore_args(ArgumentArray& cmd, SliceArray& keys,
			WeightArray& ws, AggregateType& type)
	{
		int numkeys;
		if (!string_toint32(cmd[1], numkeys) || numkeys <= 0)
		{
			return false;
		}
		if (cmd.size() < (uint32) numkeys + 2)
		{
			return false;
		}
		for (int i = 2; i < numkeys + 2; i++)
		{
			keys.push_back(cmd[i]);
		}
		if (cmd.size() > (uint32) numkeys + 2)
		{
			uint32 idx = numkeys + 2;
			std::string opstr = string_tolower(cmd[numkeys + 2]);
			if (opstr == "weights")
			{
				if (cmd.size() < ((uint32) numkeys * 2) + 3)
				{
					return false;
				}
				uint32 weight;
				for (int i = idx + 1; i < (numkeys * 2) + 3; i++)
				{
					if (!string_touint32(cmd[i], weight))
					{
						return false;
					}
					ws.push_back(weight);
				}
				idx = numkeys * 2 + 3;
				if (cmd.size() <= idx)
				{
					return true;
				}
				opstr = string_tolower(cmd[idx]);
			}
			if (cmd.size() > (idx + 1) && opstr == "aggregate")
			{
				std::string typestr = string_tolower(cmd[idx + 1]);
				if (typestr == "sum")
				{
					type = AGGREGATE_SUM;
				} else if (typestr == "max")
				{
					type = AGGREGATE_MAX;
				} else if (typestr == "min")
				{
					type = AGGREGATE_MIN;
				} else
				{
					return false;
				}
			} else
			{
				return false;
			}
		}
		return true;
	}

	int ArdbServer::ZInterStore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		WeightArray ws;
		AggregateType type = AGGREGATE_SUM;
		if (!process_zstore_args(cmd.GetArguments(), keys, ws, type))
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		int count = m_db->ZInterStore(ctx.currentDB, cmd.GetArguments()[0],
				keys, ws, type);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::ZUnionStore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray keys;
		WeightArray ws;
		AggregateType type = AGGREGATE_SUM;
		if (!process_zstore_args(cmd.GetArguments(), keys, ws, type))
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		int count = m_db->ZUnionStore(ctx.currentDB, cmd.GetArguments()[0],
				keys, ws, type);
		fill_int_reply(ctx.reply, count);
		return 0;
	}

	int ArdbServer::ZScore(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		double score;
		int ret = m_db->ZScore(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], score);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_double_reply(ctx.reply, score);
		}
		return 0;
	}

//=========================Lists cmds================================
	int ArdbServer::LIndex(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int index;
		if (!string_toint32(cmd.GetArguments()[1], index))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		std::string v;
		int ret = m_db->LIndex(ctx.currentDB, cmd.GetArguments()[0], index, v);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_str_reply(ctx.reply, v);
		}
		return 0;
	}

	int ArdbServer::LInsert(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->LInsert(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], cmd.GetArguments()[2],
				cmd.GetArguments()[3]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::LLen(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->LLen(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::LPop(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string v;
		int ret = m_db->LPop(ctx.currentDB, cmd.GetArguments()[0], v);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_str_reply(ctx.reply, v);
		}
		return 0;
	}
	int ArdbServer::LPush(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int count = 0;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			count = m_db->LPush(ctx.currentDB, cmd.GetArguments()[0],
					cmd.GetArguments()[i]);
		}
		if (count < 0)
		{
			count = 0;
		}
		fill_int_reply(ctx.reply, count);
		return 0;
	}
	int ArdbServer::LPushx(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->LPushx(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::LRange(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int start, stop;
		if (!string_toint32(cmd.GetArguments()[1], start)
				|| !string_toint32(cmd.GetArguments()[2], stop))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		ValueArray vs;
		m_db->LRange(ctx.currentDB, cmd.GetArguments()[0], start, stop, vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}
	int ArdbServer::LRem(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int count;
		if (!string_toint32(cmd.GetArguments()[1], count))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->LRem(ctx.currentDB, cmd.GetArguments()[0], count,
				cmd.GetArguments()[2]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}
	int ArdbServer::LSet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int index;
		if (!string_toint32(cmd.GetArguments()[1], index))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		int ret = m_db->LSet(ctx.currentDB, cmd.GetArguments()[0], index,
				cmd.GetArguments()[2]);
		if (ret < 0)
		{
			fill_error_reply(ctx.reply, "ERR index out of range");
		} else
		{
			fill_status_reply(ctx.reply, "OK");
		}
		return 0;
	}

	int ArdbServer::LTrim(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int start, stop;
		if (!string_toint32(cmd.GetArguments()[1], start)
				|| !string_toint32(cmd.GetArguments()[2], stop))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		m_db->LTrim(ctx.currentDB, cmd.GetArguments()[0], start, stop);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::RPop(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string v;
		int ret = m_db->RPop(ctx.currentDB, cmd.GetArguments()[0], v);
		if (ret < 0)
		{
			ctx.reply.type = REDIS_REPLY_NIL;
		} else
		{
			fill_str_reply(ctx.reply, v);
		}
		return 0;
	}
	int ArdbServer::RPush(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int count = 0;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			count = m_db->RPush(ctx.currentDB, cmd.GetArguments()[0],
					cmd.GetArguments()[i]);
		}
		if (count < 0)
		{
			count = 0;
		}
		fill_int_reply(ctx.reply, count);
		return 0;
	}
	int ArdbServer::RPushx(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int ret = m_db->RPushx(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_int_reply(ctx.reply, ret);
		return 0;
	}

	int ArdbServer::RPopLPush(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string v;
		m_db->RPopLPush(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1], v);
		fill_str_reply(ctx.reply, v);
		return 0;
	}

//=========================Tables cmds================================
	int ArdbServer::TCreate(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		SliceArray ks;
		for (uint32 i = 1; i < cmd.GetArguments().size(); i++)
		{
			ks.push_back(cmd.GetArguments()[i]);
		}
		m_db->TCreate(ctx.currentDB, cmd.GetArguments()[0], ks);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::TLen(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		int len = m_db->TCount(ctx.currentDB, cmd.GetArguments()[0]);
		fill_int_reply(ctx.reply, len);
		return 0;
	}

	int ArdbServer::TGetAll(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		ValueArray vs;
		m_db->TGetAll(ctx.currentDB, cmd.GetArguments()[0], vs);
		fill_array_reply(ctx.reply, vs);
		return 0;
	}

	int ArdbServer::TGet(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		TableQueryOptions options;
		if (!TableQueryOptions::Parse(cmd.GetArguments(), 1, options))
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		ValueArray vs;
		std::string err;
		int ret = m_db->TGet(ctx.currentDB, cmd.GetArguments()[0], options, vs,
				err);
		if (ret == 0)
		{
			fill_array_reply(ctx.reply, vs);
		} else
		{
			fill_error_reply(ctx.reply, "%s", err.c_str());
		}
		return 0;
	}
	int ArdbServer::TUpdate(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		TableUpdateOptions options;
		if (!TableUpdateOptions::Parse(cmd.GetArguments(), 1, options))
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		int ret = m_db->TUpdate(ctx.currentDB, cmd.GetArguments()[0], options);
		if (ret >= 0)
		{
			fill_int_reply(ctx.reply, ret);
		} else
		{
			fill_error_reply(ctx.reply, "ERR no record found for condition");
		}
		return 0;
	}

	int ArdbServer::TCreateIndex(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->TCreateIndex(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}

	int ArdbServer::TDelCol(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		m_db->TDelCol(ctx.currentDB, cmd.GetArguments()[0],
				cmd.GetArguments()[1]);
		fill_status_reply(ctx.reply, "OK");
		return 0;
	}
	int ArdbServer::TDel(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		TableDeleteOptions options;
		if (!TableDeleteOptions::Parse(cmd.GetArguments(), 1, options))
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		std::string err;
		int ret = m_db->TDel(ctx.currentDB, cmd.GetArguments()[0], options,
				err);
		if (ret >= 0)
		{
			fill_int_reply(ctx.reply, ret);
		} else
		{
			fill_error_reply(ctx.reply, "%s", err.c_str());
		}
		return 0;
	}

	int ArdbServer::TDesc(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		std::string str;
		int ret = m_db->TDesc(ctx.currentDB, cmd.GetArguments()[0], str);
		if (ret == 0)
		{
			fill_str_reply(ctx.reply, str);
		} else
		{
			fill_error_reply(ctx.reply, "ERR Not Found");
		}
		return 0;
	}

	int ArdbServer::TInsert(ArdbConnContext& ctx, RedisCommandFrame& cmd)
	{
		TableInsertOptions options;
		if (!TableInsertOptions::Parse(cmd.GetArguments(), 1, options))
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		std::string err;
		int ret = m_db->TInsert(ctx.currentDB, cmd.GetArguments()[0], options,
				cmd.GetCommand() == "treplace", err);
		if (ret != 0)
		{
			ctx.reply.str = err;
			ctx.reply.type = REDIS_REPLY_ERROR;
		} else
		{
			fill_status_reply(ctx.reply, "OK");
		}
		return 0;
	}

	ArdbServer::RedisCommandHandlerSetting * ArdbServer::FindRedisCommandHandlerSetting(
			std::string & cmd)
	{
		RedisCommandHandlerSettingTable::iterator found = m_handler_table.find(
				cmd);
		if (found != m_handler_table.end())
		{
			return &(found->second);
		}
		return NULL;
	}

//	static bool is_sort_write_cmd(RedisCommandFrame& cmd)
//	{
//		if (cmd.GetCommand() == "sort")
//		{
//			ArgumentArray::iterator it = cmd.GetArguments().begin();
//			while (it != cmd.GetArguments().end())
//			{
//				if (*it == "store")
//				{
//					return true;
//				}
//				it++;
//			}
//		}
//		return false;
//	}

	void ArdbServer::ProcessRedisCommand(ArdbConnContext& ctx,
			RedisCommandFrame& args)
	{
		m_ctx_local.SetValue(&ctx);
		if (m_cfg.timeout > 0)
		{
			TouchIdleConn(ctx.conn);
		}
		std::string& cmd = args.GetCommand();
		lower_string(cmd);
		RedisCommandHandlerSetting* setting = FindRedisCommandHandlerSetting(
				cmd);
		DEBUG_LOG("Process recved cmd:%s", cmd.c_str());
		kServerStat.IncRecvReq();
		int ret = 0;
		if (NULL != setting)
		{
			args.SetType(setting->type);
			/**
			 * Return error if ardb is saving data for all cmds(leveldb may also change files when reading)
			 */
			if (m_repli_serv.IsSavingData())
			{
				fill_error_reply(ctx.reply, "ERR server is saving data");
				goto _exit;
			}
			bool valid_cmd = true;
			if (setting->min_arity > 0)
			{
				valid_cmd = args.GetArguments().size()
						>= (uint32) setting->min_arity;
			}
			if (setting->max_arity >= 0 && valid_cmd)
			{
				valid_cmd = args.GetArguments().size()
						<= (uint32) setting->max_arity;
			}

			if (!valid_cmd)
			{
				fill_error_reply(ctx.reply,
						"ERR wrong number of arguments for '%s' command",
						cmd.c_str());
			} else
			{
				if (ctx.IsInTransaction()
						&& (cmd != "multi" && cmd != "exec" && cmd != "discard"
								&& cmd != "quit"))
				{
					ctx.transaction_cmds->push_back(args);
					fill_status_reply(ctx.reply, "QUEUED");
					ctx.conn->Write(ctx.reply);
					return;
				} else if (ctx.IsSubscribedConn()
						&& (cmd != "subscribe" && cmd != "psubscribe"
								&& cmd != "unsubscribe" && cmd != "punsubscribe"
								&& cmd != "quit"))
				{
					fill_error_reply(ctx.reply,
							"ERR only (P)SUBSCRIBE / (P)UNSUBSCRIBE / QUIT allowed in this context");
				} else
				{
					ret = DoRedisCommand(ctx, setting, args);
				}
			}
		} else
		{
			ERROR_LOG("No handler found for:%s", cmd.c_str());
			fill_error_reply(ctx.reply, "ERR unknown command '%s'",
					cmd.c_str());
		}

		_exit: if (ctx.reply.type != 0)
		{
			kServerStat.IncSentReply();
			ctx.conn->Write(ctx.reply);
			ctx.reply.Clear();
		}
		if (ret < 0)
		{
			ctx.conn->Close();
		}
	}

	int ArdbServer::DoRedisCommand(ArdbConnContext& ctx,
			RedisCommandHandlerSetting* setting, RedisCommandFrame& args)
	{
		std::string& cmd = args.GetCommand();
		if (m_clients_holder.IsStatEnable())
		{
			m_clients_holder.TouchConn(ctx.conn, cmd);
		}
		uint64 start_time = get_current_epoch_micros();
		int ret = (this->*(setting->handler))(ctx, args);
		uint64 stop_time = get_current_epoch_micros();

		if (m_cfg.slowlog_log_slower_than
				&& (stop_time - start_time)
						> (uint64) m_cfg.slowlog_log_slower_than)
		{
			m_slowlog_handler.PushSlowCommand(args, stop_time - start_time);
		}
		return ret;
	}

	static void conn_pipeline_init(ChannelPipeline* pipeline, void* data)
	{
		ArdbServer* serv = (ArdbServer*) data;
		pipeline->AddLast("decoder", new RedisCommandDecoder);
		pipeline->AddLast("encoder", new RedisReplyEncoder);
		pipeline->AddLast("handler", new RedisRequestHandler(serv));
	}

	static void conn_pipeline_finallize(ChannelPipeline* pipeline, void* data)
	{
		ChannelHandler* handler = pipeline->Get("decoder");
		DELETE(handler);
		handler = pipeline->Get("encoder");
		DELETE(handler);
		handler = pipeline->Get("handler");
		DELETE(handler);
	}

	void RedisRequestHandler::MessageReceived(ChannelHandlerContext& ctx,
			MessageEvent<RedisCommandFrame>& e)
	{
		ardbctx.conn = ctx.GetChannel();
		server->ProcessRedisCommand(ardbctx, *(e.GetMessage()));
	}
	void RedisRequestHandler::ChannelClosed(ChannelHandlerContext& ctx,
			ChannelStateEvent& e)
	{
		server->m_clients_holder.EraseConn(ctx.GetChannel());
		server->ClearWatchKeys(ardbctx);
		server->ClearSubscribes(ardbctx);
	}

	void RedisRequestHandler::ChannelConnected(ChannelHandlerContext& ctx,
			ChannelStateEvent& e)
	{
		if (server->m_cfg.timeout > 0)
		{
			server->TouchIdleConn(ctx.GetChannel());
		}
	}

	static void daemonize(void)
	{
		int fd;

		if (fork() != 0)
		{
			exit(0); /* parent exits */
		}
		setsid(); /* create a new session */

		if ((fd = open("/dev/null", O_RDWR, 0)) != -1)
		{
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
			{
				close(fd);
			}
		}
	}

	Timer & ArdbServer::GetTimer()
	{
		return m_service->GetTimer();
	}

	int ArdbServer::Start(const Properties& props)
	{
		m_cfg_props = props;
		if (ParseConfig(props, m_cfg) < 0)
		{
			ERROR_LOG("Failed to parse configurations.");
			return -1;
		}
		if (m_cfg.daemonize)
		{
			daemonize();
		}

		//m_engine = new SelectedDBEngineFactory(props);
		m_db = new Ardb(&m_engine, m_cfg.worker_count > 1);
		if (!m_db->Init())
		{
			ERROR_LOG("Failed to init DB.");
			return -1;
		}
		m_service = new ChannelService(m_cfg.max_clients + 32);

		ChannelOptions ops;
		ops.tcp_nodelay = true;
		if (m_cfg.tcp_keepalive > 0)
		{
			ops.keep_alive = m_cfg.tcp_keepalive;
		}
		if (m_cfg.listen_host.empty() && m_cfg.listen_unix_path.empty())
		{
			m_cfg.listen_host = "0.0.0.0";
			if (m_cfg.listen_port == 0)
			{
				m_cfg.listen_port = 6379;
			}
		}

		if (!m_cfg.listen_host.empty())
		{
			SocketHostAddress address(m_cfg.listen_host.c_str(),
					m_cfg.listen_port);
			ServerSocketChannel* server = m_service->NewServerSocketChannel();
			if (!server->Bind(&address))
			{
				ERROR_LOG("Failed to bind on %s:%d", m_cfg.listen_host.c_str(),
						m_cfg.listen_port);
				goto sexit;
			}
			server->Configure(ops);
			server->SetChannelPipelineInitializor(conn_pipeline_init, this);
			server->SetChannelPipelineFinalizer(conn_pipeline_finallize, NULL);
		}
		if (!m_cfg.listen_unix_path.empty())
		{
			SocketUnixAddress address(m_cfg.listen_unix_path);
			ServerSocketChannel* server = m_service->NewServerSocketChannel();
			if (!server->Bind(&address))
			{
				ERROR_LOG("Failed to bind on %s",
						m_cfg.listen_unix_path.c_str());
				goto sexit;
			}
			server->Configure(ops);
			server->SetChannelPipelineInitializor(conn_pipeline_init, this);
			server->SetChannelPipelineFinalizer(conn_pipeline_finallize, NULL);
			chmod(m_cfg.listen_unix_path.c_str(), m_cfg.unixsocketperm);
		}
		ArdbLogger::InitDefaultLogger(m_cfg.loglevel, m_cfg.logfile);

		if (m_cfg.repl_log_enable)
		{
			m_repli_serv.Init();
			m_repli_serv.Start();
			m_db->RegisterRawKeyListener(&m_repli_serv);
		}
		if (!m_cfg.master_host.empty())
		{
			m_slave_client.SetSyncDBs(m_cfg.syncdbs);
			m_slave_client.ConnectMaster(m_cfg.master_host, m_cfg.master_port);
		}
		m_service->SetThreadPoolSize(m_cfg.worker_count);
		INFO_LOG("Server started, Ardb version %s", ARDB_VERSION);
		INFO_LOG("The server is now ready to accept connections on port %d",
				m_cfg.listen_port);
		m_service->Start();
		sexit: m_repli_serv.Stop();
		DELETE(m_db);
		DELETE(m_service);
		ArdbLogger::DestroyDefaultLogger();
		return 0;
	}
}

