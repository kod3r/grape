/*
	Copyright (c) 2013+ Ruslan Nigmatullin <euroelessar@yandex.ru>

	This file is part of Grape.

	Cocaine is free software; you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	Cocaine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <time.h>

#include <cocaine/context.hpp>
#include <cocaine/logging.hpp>
#include <cocaine/api/event.hpp>

#include <cocaine/app.hpp>
#include <cocaine/exceptions.hpp>
#include <cocaine/api/event.hpp>
#include <cocaine/api/stream.hpp>
#include <cocaine/api/service.hpp>

#include "cocaine-json-trait.hpp"

#include "grape/rapidjson/document.h"

#include <grape/data_array.hpp>
#include <grape/entry_id.hpp>

#include "driver.hpp"

using namespace cocaine::driver;

queue_driver::queue_driver(cocaine::context_t& context, cocaine::io::reactor_t &reactor, cocaine::app_t &app,
		const std::string& name, const Json::Value& args)
	: category_type(context, reactor, app, name, args)
	, m_context(context)
	, m_app(app)
	, m_log(new cocaine::logging::log_t(context, cocaine::format("driver/%s", name)))
	, m_idle_timer(reactor.native())
	, m_queue_name(args.get("source-queue-app", "queue").asString())
	, m_worker_event(args.get("worker-emit-event", "emit").asString())
	, m_queue_pop_event(m_queue_name + "@" + args.get("source-queue-pop-event", "pop-multiple-string").asString())
	, m_timeout(args.get("timeout", 0.0f).asDouble())
	, m_deadline(args.get("deadline", 0.0f).asDouble())
	, m_queue_length(0)
	, m_queue_length_max(0)
	, m_factor(0)
	, m_queue_src_key(0)
{
	COCAINE_LOG_INFO(m_log, "init: %s driver", m_queue_name.c_str());

	srand(time(NULL));

	try {
		std::string s = Json::FastWriter().write(args);

		rapidjson::Document doc;
		doc.Parse<0>(s.c_str());

		m_client = elliptics_client_state::create(doc);

		//TODO: remove extra code with queue-groups: driver doesn't need group set override
		// 
		std::string groups_key = "groups";
		if (doc.HasMember("queue-groups"))
			groups_key = "queue-groups";

		const rapidjson::Value &groupsArray = doc[groups_key.c_str()];
		std::transform(groupsArray.Begin(), groupsArray.End(), std::back_inserter(m_queue_groups),
				std::bind(&rapidjson::Value::GetInt, std::placeholders::_1));

		COCAINE_LOG_INFO(m_log, "init: elliptics client created");

	} catch (const std::exception &e) {
		COCAINE_LOG_ERROR(m_log, "init: %s driver constructor exception: %s", m_queue_name.c_str(), e.what());
		throw;
	}

	if (m_queue_name.empty())
		throw configuration_error("no queue name has been specified");

	char *ptr = strchr((char *)m_worker_event.c_str(), '@');
	if (!ptr)
		throw configuration_error("invalid worker event ('emit' config entry), it must contain @ sign");

	std::string app_name(m_worker_event.c_str(), ptr - m_worker_event.c_str());
	std::string event_name(ptr+1);

	auto storage = cocaine::api::storage(context, "core");
	Json::Value profile = storage->get<Json::Value>("profiles", app_name);
	int queue_limit = profile["queue-limit"].asInt();

	m_queue_length_max = queue_limit * 9 / 10;

	m_idle_timer.set<queue_driver, &queue_driver::on_idle_timer_event>(this);
	m_idle_timer.set(0.0f, 1.0f);
	m_idle_timer.again();

	COCAINE_LOG_INFO(m_log, "init: %s driver started", m_queue_name.c_str());
}

queue_driver::~queue_driver()
{
	m_idle_timer.stop();
}

Json::Value queue_driver::info() const
{
	Json::Value result;

	result["type"] = "persistent-queue";
	result["name"] = m_queue_name;
	result["queue-stats"]["inserted"] = (int)m_queue_length;
	result["queue-stats"]["max-length"] = (int)m_queue_length_max;

	return result;
}

void queue_driver::on_idle_timer_event(ev::timer &, int)
{
	COCAINE_LOG_INFO(m_log, "%s: timer: checking queue completed: queue-len: %d/%d",
			m_queue_name.c_str(), m_queue_length, m_queue_length_max);

	get_more_data();
}

void queue_driver::get_more_data()
{
	COCAINE_LOG_INFO(m_log, "%s: more-data: checking queue: queue-len: %d/%d",
			m_queue_name.c_str(), m_queue_length, m_queue_length_max);

	int num = m_queue_length_max - m_queue_length;
	if (num < 100)
		return;
	
	int step = 10;

	for (int i = 0; i < step; ++i) {
		ioremap::elliptics::session sess = m_client.create_session();

		std::shared_ptr<queue_request> req = std::make_shared<queue_request>();
		req->num = m_queue_length_max / 10;
		req->src_key = m_queue_src_key;

		req->id.group_id = 0;

		std::string random_data = m_queue_name + std::to_string(req->src_key) + std::to_string(rand());
		sess.transform(random_data, req->id);

		sess.set_groups(m_queue_groups);

		queue_inc(1);

		sess.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
		sess.exec(&req->id, req->src_key, m_queue_pop_event, std::to_string(req->num)).connect(
			std::bind(&queue_driver::on_queue_request_data, this, req, std::placeholders::_1),
			std::bind(&queue_driver::on_queue_request_complete, this, req, std::placeholders::_1)
		);

		++m_queue_src_key;

		COCAINE_LOG_INFO(m_log, "%s: %s: pop request has been sent: requested number of events: %d, queue-len: %d/%d",
				m_queue_name.c_str(), dnet_dump_id(&req->id), req->num, m_queue_length, m_queue_length_max);
	}
}

void queue_driver::on_queue_request_data(std::shared_ptr<queue_request> req, const ioremap::elliptics::exec_result_entry &result)
{
	try {
		if (result.error()) {
			COCAINE_LOG_ERROR(m_log, "%s: error: %d: %s",
				m_queue_name.c_str(), result.error().code(), result.error().message());
			return;
		}

		// queue.pop returns no data when queue is empty.
		// Idle timer handles queue emptiness firing periodic check,
		// but every time when we actually got data we have to postpone
		// the idle timer further into the future.
		m_idle_timer.again();

		ioremap::elliptics::exec_context context = result.context();

		// Received context is passed directly to the worker, so that
		// worker could use it to continue talking to the same queue instance.
		//
		// But before that context.src_key must be restored back
		// to the original src_key used in the original request to the queue,
		// or else our worker's ack will not be routed to the exact same
		// queue worker that issued reply with this context.
		//
		// (src_key of the request gets replaced by job id server side,
		// so reply does not carries the same src_key as a request.
		// Which is unfortunate.)
		context.set_src_key(req->src_key);

		COCAINE_LOG_INFO(m_log, "%s: %s: src-key: %d, data-size: %d",
				m_queue_name.c_str(), dnet_dump_id(&req->id),
				context.src_key(), context.data().size()
				);

		if (!context.data().empty()) {
			// ioremap::grape::entry_id entry_id = ioremap::grape::entry_id::from_dnet_raw_id(context.src_id());
			// COCAINE_LOG_INFO(m_log, "%s: %s: id: %d-%d, size: %d", m_queue_name.c_str(), dnet_dump_id(&req->id),
			// 		entry_id.chunk, entry_id.pos,
			// 		context.data().size()
			// 		);

			bool processed = process_data(context);

			COCAINE_LOG_INFO(m_log, "%s: %s: src-key: %d, data-size: %d, processed %d",
					m_queue_name.c_str(), dnet_dump_id(&req->id),
					context.src_key(), context.data().size(),
					processed
					);
		}

	} catch(const std::exception &e) {
		COCAINE_LOG_ERROR(m_log, "%s: %s: exception: %s", m_queue_name.c_str(), dnet_dump_id(&req->id), e.what());
	}
}

void queue_driver::on_queue_request_complete(std::shared_ptr<queue_request> req, const ioremap::elliptics::error_info &error)
{
	if (error) {
		COCAINE_LOG_ERROR(m_log, "%s: %s: queue request completion error: %s",
				m_queue_name.c_str(), dnet_dump_id(&req->id), error.message().c_str());
	} else {
		COCAINE_LOG_INFO(m_log, "%s: %s: queue request completed",
				m_queue_name.c_str(), dnet_dump_id(&req->id));
	}

	queue_dec(1);
}

bool queue_driver::process_data(const ioremap::elliptics::exec_context &context)
{
	// Pass data to the worker, return it back to the local queue if failed.

	try {
		auto downstream = std::make_shared<downstream_t>(this, context.data());

		// this map should be used to store iteration counter
		//m_events.insert(std::make_pair(static_cast<const int>(sph->src_key), data.to_string()));

		ioremap::elliptics::data_pointer packet = context.native_data();
		api::policy_t policy(false, m_timeout, m_deadline);
		auto upstream = m_app.enqueue(api::event_t(m_worker_event, policy), downstream);
		upstream->write((char *)packet.data(), packet.size());

		return true;

	} catch (const cocaine::error_t &e) {
		COCAINE_LOG_ERROR(m_log, "%s: enqueue failed: %s: queue-len: %d/%d",
				m_queue_name.c_str(), e.what(),
				m_queue_length, m_queue_length_max);
	}

	return false;
}

void queue_driver::queue_dec(int num)
{
	m_queue_length -= num;
}

void queue_driver::queue_inc(int num)
{
	m_queue_length += num;
}

queue_driver::downstream_t::downstream_t(queue_driver *queue, const ioremap::elliptics::data_pointer &d)
	: m_queue(queue), m_data(d), m_attempts(0)
{
	m_queue->queue_inc(1);
}

queue_driver::downstream_t::~downstream_t()
{
}

void queue_driver::downstream_t::write(const char *data, size_t size)
{
	std::string ret(data, size);

	COCAINE_LOG_INFO(m_queue->m_log, "%s: from worker: received: size: %d, data: '%s'",
			m_queue->m_queue_name.c_str(), ret.size(), ret.c_str());
}

void queue_driver::downstream_t::error(int code, const std::string &msg)
{
	++m_attempts;

	COCAINE_LOG_ERROR(m_queue->m_log, "%s: from worker: error: attempts: %d: %s [%d]",
			m_queue->m_queue_name.c_str(), m_attempts, msg.c_str(), code);
}

void queue_driver::downstream_t::close()
{
	m_queue->queue_dec(1);

	COCAINE_LOG_INFO(m_queue->m_log, "%s: downstream: close: attempts (was-error): %d",
			m_queue->m_queue_name.c_str(), m_attempts);

	if (m_attempts == 0) {
		// pour way to limit exponential growth of number of requests 
		// if (++m_queue->m_factor == 10) {
		// 	m_queue->m_factor = 0;
		// 	m_queue->get_more_data();
		// }
		m_queue->get_more_data();
	}
}
