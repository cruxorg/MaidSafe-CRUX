///////////////////////////////////////////////////////////////////////////////
//
// Copyright (C) 2014 MaidSafe.net Limited
//
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)
//
///////////////////////////////////////////////////////////////////////////////

#ifndef MAIDSAFE_CRUX_DETAIL_TRANSMIT_QUEUE_HPP
#define MAIDSAFE_CRUX_DETAIL_TRANSMIT_QUEUE_HPP

#include <map>
#include <maidsafe/crux/detail/sequence_number.hpp>
#include <maidsafe/crux/detail/timer.hpp>
#include <maidsafe/crux/detail/constants.hpp>

namespace maidsafe { namespace crux { namespace detail {

// FIXME: I'm not sure about the nomenclature here, feel free to change it.

template<typename Index> class transmit_queue {
private:
    using index_type = Index;
    using duration_type = typename detail::timer::duration_type;

public:
    using iteration_handler = std::function<void(const boost::system::error_code&, std::size_t)>;
    using iteration_step    = std::function<void(iteration_handler)>;

private:
    struct entry_type {
        std::size_t       buffer_size;
        duration_type     period;
        iteration_step    step;
        iteration_handler handler;
    };

    // Shared pointer used because lambdas don't support move semantics in c++11
    using entries_type = std::map<index_type, std::shared_ptr<entry_type>>;

public:
    transmit_queue(boost::asio::io_service&);

    void push( index_type
             , std::size_t buffer_size
             , iteration_step
             , iteration_handler);

    void apply_ack(index_type);

    void shutdown();

    bool empty() const;
    std::size_t size() const;

private:
    void on_timer_tick();
    void start_step(typename entries_type::iterator);

private:
    boost::asio::io_service&       ios;
    entries_type                   entries;
    detail::timer                  timer;
    std::shared_ptr<boost::none_t> shutdown_indicator;
};

template<typename Index>
transmit_queue<Index>::transmit_queue(boost::asio::io_service& ios)
    : ios(ios)
    , timer(ios, [=]() { on_timer_tick(); })
    , shutdown_indicator(std::make_shared<boost::none_t>())
{ }

template<typename Index>
void transmit_queue<Index>::on_timer_tick()
{
    if (entries.empty()) {
        return;
    }

    start_step(entries.begin());
}

template<typename Index>
bool transmit_queue<Index>::empty() const {
    return entries.empty();
}

template<typename Index>
std::size_t transmit_queue<Index>::size() const {
    return entries.size();
}

template<typename Index>
void transmit_queue<Index>::apply_ack(index_type index)
{
    auto entry_i = entries.find(index);

    if (entry_i == entries.end()) {
        return;
    }

    bool is_active = entry_i == entries.begin();

    std::shared_ptr<entry_type> entry = std::move(entry_i->second);

    entries.erase(entry_i);

    if (is_active) {
        timer.stop();

        if (!entries.empty()) {
            start_step(entries.begin());
        }
    }

    entry->handler(boost::system::error_code(), entry->buffer_size);
}

template<typename Index>
void transmit_queue<Index>::shutdown() {
    shutdown_indicator.reset();

    timer.stop();

    auto moved_entries = std::move(entries);

    for (auto& entry_pair : moved_entries) {
        auto entry = std::move(entry_pair.second);

        ios.post([entry]() {
            entry->handler(boost::asio::error::operation_aborted, entry->buffer_size);
            });
    }
}

template<typename Index>
void transmit_queue<Index>::push( index_type        index
                                , std::size_t       buffer_size
                                , iteration_step    step
                                , iteration_handler handler)
{
    bool was_empty = entries.empty();

    auto insert_result = entries.insert(std::make_pair(index, std::make_shared<entry_type>()));

    if (!insert_result.second) {
        return ios.post([=]() {
                handler(boost::asio::error::already_started, 0);
                });
    }

    auto& entry       = *insert_result.first->second;
    entry.buffer_size = buffer_size;
    entry.period      = constant::initial_roundtrip_time;
    entry.step        = std::move(step);
    entry.handler     = std::move(handler);

    if (was_empty) {
        start_step(insert_result.first);
    }
}

template<typename Index>
void transmit_queue<Index>::start_step(typename entries_type::iterator entry_i) {
    auto entry = entry_i->second;

    std::weak_ptr<boost::none_t> shutdown_guard = shutdown_indicator;

    entry->step([=]( const boost::system::error_code& error
                   , std::size_t bytes_transferred) {
                   if (!shutdown_guard.lock() || error) {
                       return entry->handler(error, bytes_transferred);
                   }
                   // FIXME: Period should be = 
                   //        max(0, entry->period - duration of this step)
                   this->timer.set_period(entry->period);
                   this->timer.start();
               });
}

}}} // namespace maidsafe::crux::detail

#endif // MAIDSAFE_CRUX_DETAIL_TRANSMIT_QUEUE_HPP
