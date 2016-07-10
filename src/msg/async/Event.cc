// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/errno.h"
#include "Event.h"

#ifdef HAVE_EPOLL
#include "EventEpoll.h"
#else
#ifdef HAVE_KQUEUE
#include "EventKqueue.h"
#else
#include "EventSelect.h"
#endif
#endif

#define dout_subsys ceph_subsys_ms

#undef dout_prefix
#define dout_prefix *_dout << "EventCallback "
class C_handle_notify : public EventCallback {
  EventCenter *center;
  CephContext *cct;

 public:
  C_handle_notify(EventCenter *c, CephContext *cc): center(c), cct(cc) {}
  void do_request(int fd_or_id) {
    char c[256];
    do {
      int r = read(fd_or_id, c, sizeof(c));
      if (r < 0) {
        if (errno != EAGAIN)
          ldout(cct, 1) << __func__ << " read notify pipe failed: " << cpp_strerror(errno) << dendl;
        break;
      }
    } while (1);
  }
};

#undef dout_prefix
#define dout_prefix _event_prefix(_dout)

ostream& EventCenter::_event_prefix(std::ostream *_dout)
{
  return *_dout << "Event(" << this << " nevent=" << nevent
                << " time_id=" << time_event_next_id << ").";
}

int EventCenter::init(int n, unsigned i)
{
  // can't init multi times
  assert(nevent == 0);

  idx = i;

#ifdef HAVE_EPOLL
  driver = new EpollDriver(cct);
#else
#ifdef HAVE_KQUEUE
  driver = new KqueueDriver(cct);
#else
  driver = new SelectDriver(cct);
#endif
#endif

  if (!driver) {
    lderr(cct) << __func__ << " failed to create event driver " << dendl;
    return -1;
  }

  int r = driver->init(n);
  if (r < 0) {
    lderr(cct) << __func__ << " failed to init event driver." << dendl;
    return r;
  }

  int fds[2];
  if (pipe(fds) < 0) {
    lderr(cct) << __func__ << " can't create notify pipe" << dendl;
    return -errno;
  }

  notify_receive_fd = fds[0];
  notify_send_fd = fds[1];
  r = net.set_nonblock(notify_receive_fd);
  if (r < 0) {
    return r;
  }
  r = net.set_nonblock(notify_send_fd);
  if (r < 0) {
    return r;
  }

  file_events.resize(n);
  nevent = n;

  cct->lookup_or_create_singleton_object<EventCenter::AssociatedCenters>(
      global_centers, "AsyncMessenger::EventCenter::global_center");
  assert(global_centers && !global_centers->centers[idx]);
  global_centers->centers[idx] = this;
  owner = pthread_self();
  ldout(cct, 1) << __func__ << " idx=" << idx << " owner=" << owner << dendl;

  notify_handler = new C_handle_notify(this, cct);
  r = create_file_event(notify_receive_fd, EVENT_READABLE, notify_handler);
  assert(r == 0);

  return 0;
}

EventCenter::~EventCenter()
{
  {
    std::lock_guard<std::mutex> l(external_lock);
    while (!external_events.empty()) {
      EventCallbackRef e = external_events.front();
      if (e)
        e->do_request(0);
      external_events.pop_front();
    }
  }
  assert(time_events.empty());

  if (notify_receive_fd >= 0)
    ::close(notify_receive_fd);
  if (notify_send_fd >= 0)
    ::close(notify_send_fd);

  delete driver;
  if (notify_handler)
    delete notify_handler;
}

int EventCenter::create_file_event(int fd, int mask, EventCallbackRef ctxt)
{
  assert(in_thread());
  int r = 0;
  if (fd >= nevent) {
    int new_size = nevent << 2;
    while (fd > new_size)
      new_size <<= 2;
    ldout(cct, 10) << __func__ << " event count exceed " << nevent << ", expand to " << new_size << dendl;
    r = driver->resize_events(new_size);
    if (r < 0) {
      lderr(cct) << __func__ << " event count is exceed." << dendl;
      return -ERANGE;
    }
    file_events.resize(new_size);
    nevent = new_size;
  }

  EventCenter::FileEvent *event = _get_file_event(fd);
  ldout(cct, 20) << __func__ << " create event started fd=" << fd << " mask=" << mask
                 << " original mask is " << event->mask << dendl;
  if (event->mask == mask)
    return 0;

  r = driver->add_event(fd, event->mask, mask);
  if (r < 0) {
    // Actually we don't allow any failed error code, caller doesn't prepare to
    // handle error status. So now we need to assert failure here. In practice,
    // add_event shouldn't report error, otherwise it must be a innermost bug!
    assert(0 == "BUG!");
    return r;
  }

  event->mask |= mask;
  if (mask & EVENT_READABLE) {
    event->read_cb = ctxt;
  }
  if (mask & EVENT_WRITABLE) {
    event->write_cb = ctxt;
  }
  ldout(cct, 10) << __func__ << " create event end fd=" << fd << " mask=" << mask
                 << " original mask is " << event->mask << dendl;
  return 0;
}

void EventCenter::delete_file_event(int fd, int mask)
{
  assert(in_thread() && fd >= 0);
  if (fd >= nevent) {
    ldout(cct, 1) << __func__ << " delete event fd=" << fd << " is equal or greater than nevent=" << nevent
                  << "mask=" << mask << dendl;
    return ;
  }
  EventCenter::FileEvent *event = _get_file_event(fd);
  ldout(cct, 20) << __func__ << " delete event started fd=" << fd << " mask=" << mask
                 << " original mask is " << event->mask << dendl;
  if (!event->mask)
    return ;

  int r = driver->del_event(fd, event->mask, mask);
  if (r < 0) {
    // see create_file_event
    assert(0 == "BUG!");
  }

  if (mask & EVENT_READABLE && event->read_cb) {
    event->read_cb = nullptr;
  }
  if (mask & EVENT_WRITABLE && event->write_cb) {
    event->write_cb = nullptr;
  }

  event->mask = event->mask & (~mask);
  ldout(cct, 10) << __func__ << " delete event end fd=" << fd << " mask=" << mask
                 << " original mask is " << event->mask << dendl;
}

uint64_t EventCenter::create_time_event(uint64_t microseconds, EventCallbackRef ctxt)
{
  assert(in_thread());
  uint64_t id = time_event_next_id++;

  ldout(cct, 10) << __func__ << " id=" << id << " trigger after " << microseconds << "us"<< dendl;
  EventCenter::TimeEvent event;
  clock_type::time_point expire = clock_type::now() + std::chrono::microseconds(microseconds);
  event.id = id;
  event.time_cb = ctxt;
  std::multimap<clock_type::time_point, TimeEvent>::value_type s_val(expire, event);
  auto it = time_events.insert(std::move(s_val));
  event_map[id] = it;

  return id;
}

void EventCenter::delete_time_event(uint64_t id)
{
  assert(in_thread());
  ldout(cct, 10) << __func__ << " id=" << id << dendl;
  if (id >= time_event_next_id || id == 0)
    return ;

  auto it = event_map.find(id);
  if (it == event_map.end()) {
    ldout(cct, 10) << __func__ << " id=" << id << " not found" << dendl;
    return ;
  }

  time_events.erase(it->second);
  event_map.erase(it);
}

void EventCenter::wakeup()
{
  ldout(cct, 1) << __func__ << dendl;

  if (!external_num_events.load()) {
    char buf[1];
    buf[0] = 'c';
    // wake up "event_wait"
    int n = write(notify_send_fd, buf, 1);
    if (n < 0) {
      ldout(cct, 1) << __func__ << " write notify pipe failed: " << cpp_strerror(errno) << dendl;
      assert(0);
    }
  }
}

int EventCenter::process_time_events()
{
  int processed = 0;
  clock_type::time_point now = clock_type::now();
  ldout(cct, 10) << __func__ << " cur time is " << now << dendl;

  while (!time_events.empty()) {
    auto it = time_events.begin();
    if (now >= it->first) {
      TimeEvent &e = it->second;
      EventCallbackRef cb = e.time_cb;
      uint64_t id = e.id;
      time_events.erase(it);
      event_map.erase(id);
      ldout(cct, 10) << __func__ << " process time event: id=" << id << dendl;
      processed++;
      cb->do_request(id);
    } else {
      break;
    }
  }

  return processed;
}

int EventCenter::process_events(int timeout_microseconds)
{
  struct timeval tv;
  int numevents;
  bool trigger_time = false;
  auto now = clock_type::now();

  // If exists external events, don't block
  if (external_num_events.load()) {
    tv.tv_sec = 0;
    tv.tv_usec = 0;
  } else {
    clock_type::time_point shortest;
    shortest = now + std::chrono::microseconds(timeout_microseconds); 

    auto it = time_events.begin();
    if (it != time_events.end() && shortest >= it->first) {
      ldout(cct, 10) << __func__ << " shortest is " << shortest << " it->first is " << it->first << dendl;
      shortest = it->first;
      trigger_time = true;
      if (shortest > now) {
        timeout_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            shortest - now).count();
      } else {
        shortest = now;
        timeout_microseconds = 0;
      }
    }
    tv.tv_sec = timeout_microseconds / 1000000;
    tv.tv_usec = timeout_microseconds % 1000000;
  }

  ldout(cct, 10) << __func__ << " wait second " << tv.tv_sec << " usec " << tv.tv_usec << dendl;
  vector<FiredFileEvent> fired_events;
  numevents = driver->event_wait(fired_events, &tv);
  for (int j = 0; j < numevents; j++) {
    int rfired = 0;
    FileEvent *event;
    EventCallbackRef cb;
    event = _get_file_event(fired_events[j].fd);

    /* note the event->mask & mask & ... code: maybe an already processed
    * event removed an element that fired and we still didn't
    * processed, so we check if the event is still valid. */
    if (event->mask & fired_events[j].mask & EVENT_READABLE) {
      rfired = 1;
      cb = event->read_cb;
      cb->do_request(fired_events[j].fd);
    }

    if (event->mask & fired_events[j].mask & EVENT_WRITABLE) {
      if (!rfired || event->read_cb != event->write_cb) {
        cb = event->write_cb;
        cb->do_request(fired_events[j].fd);
      }
    }

    ldout(cct, 20) << __func__ << " event_wq process is " << fired_events[j].fd << " mask is " << fired_events[j].mask << dendl;
  }

  if (trigger_time)
    numevents += process_time_events();

  if (external_num_events.load()) {
    external_lock.lock();
    if (external_events.empty()) {
      external_lock.unlock();
    } else {
      deque<EventCallbackRef> cur_process;
      cur_process.swap(external_events);
      external_num_events.store(0);
      external_lock.unlock();
      while (!cur_process.empty()) {
        EventCallbackRef e = cur_process.front();
        ldout(cct, 20) << __func__ << " do " << e << dendl;
        if (e)
          e->do_request(0);
        cur_process.pop_front();
        numevents++;
      }
    }
  }
  return numevents;
}

void EventCenter::dispatch_event_external(EventCallbackRef e)
{
  external_lock.lock();
  external_events.push_back(e);
  uint64_t num = ++external_num_events;
  external_lock.unlock();
  if (!in_thread())
    wakeup();

  ldout(cct, 20) << __func__ << " " << e << " pending " << num << dendl;
}
