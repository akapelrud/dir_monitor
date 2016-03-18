//
// Based on DirectoryWatcher.cpp, original license terms follow.
//
// $Id: //poco/1.4/Foundation/src/DirectoryWatcher.cpp#4 $
//
// Library: Foundation
// Package: Filesystem
// Module:  DirectoryWatcher
//
// Copyright (c) 2012, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
//
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
#pragma once

#include <boost/filesystem.hpp>
#include <boost/ptr_container/ptr_unordered_map.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <string>
#include <deque>

#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/array.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

namespace boost {
namespace asio {

class dir_monitor_impl :
{
    class unix_handle
        : public boost::noncopyable
    {
    public:
        unix_handle(int handle)
            : handle_(handle)
        {
        }

        ~unix_handle()
        {
            ::close(handle_);
        }

        operator int() const { return handle_; }

    private:
        int handle_;
    };

    typedef std::map<std::string, boost::filesystem::directory_entry> dir_entry_map;

public:
    dir_monitor_impl()
        : kqueue_(init_kqueue())
        , run_(true)
        , work_thread_(&boost::asio::dir_monitor_impl::work_thread, this)
    {}

    ~dir_monitor_impl()
    {
        // The work thread is stopped and joined.
        stop_work_thread();
        work_thread_.join();
        ::close(kqueue_);
    }

    void add_directory(std::string dirname, int wd)
    {
        boost::unique_lock<boost::mutex> lock(dirs_mutex_);
        dirs_.insert(dirname, new unix_handle(wd));
        scan(dirname, entries[dirname]);
    }

    void remove_directory(const std::string &dirname)
    {
        boost::unique_lock<boost::mutex> lock(dirs_mutex_);
        dirs_.erase(dirname);
    }

    void destroy()
    {
        boost::unique_lock<boost::mutex> lock(events_mutex_);
        run_ = false;
        events_cond_.notify_all();
    }

    dir_monitor_event popfront_event(boost::system::error_code &ec)
    {
        boost::unique_lock<boost::mutex> lock(events_mutex_);
        while (run_ && events_.empty()) {
            events_cond_.wait(lock);
        }
        dir_monitor_event ev;
        if (!events_.empty())
        {
            ec = boost::system::error_code();
            ev = events_.front();
            events_.pop_front();
        }
        else
            ec = boost::asio::error::operation_aborted;
        return ev;
    }

    void pushback_event(dir_monitor_event ev)
    {
        boost::unique_lock<boost::mutex> lock(events_mutex_);
        if (run_)
        {
            events_.push_back(ev);
            events_cond_.notify_all();
        }
    }

private:
    int init_kqueue()
    {
        int fd = kqueue();
        if (fd == -1)
        {
            boost::system::system_error e(boost::system::error_code(errno, boost::system::get_system_category()), "boost::asio::dir_monitor_impl::init_kqueue: kqueue failed");
            boost::throw_exception(e);
        }
        return fd;
    }

    void scan(std::string const& dir, dir_entry_map& entries)
    {
        boost::system::error_code ec;
        boost::filesystem::recursive_directory_iterator it(dir, ec);
        boost::filesystem::recursive_directory_iterator end;

        if (ec)
        {
            boost::system::system_error e(ec, "boost::asio::dir_monitor_impl::scan: unable to iterate directories");
            boost::throw_exception(e);
        }

        while (it != end)
        {
            entries[(*it).path().native()] = *it;
            ++it;
        }
    }

    void compare(std::string const& dir, dir_entry_map& old_entries, dir_entry_map& new_entries)
    {
        // @todo filename() loses path relative to dir
        // Need to construct with dir as a root_path() and get all paths relative to that.
        for (dir_entry_map::iterator itn = new_entries.begin(); itn != new_entries.end(); ++itn)
        {
            dir_entry_map::iterator ito = old_entries.find(itn->first);
            if (ito != old_entries.end())
            {
                if (!boost::filesystem::equivalent(itn->second.path(), ito->second.path()) or
                    boost::filesystem::last_write_time(itn->second.path()) != boost::filesystem::last_write_time(ito->second.path()) or
                    (boost::filesystem::is_regular_file(itn->second.path()) and boost::filesystem::is_regular_file(ito->second.path()) and
                    boost::filesystem::file_size(itn->second.path()) != boost::filesystem::file_size(ito->second.path())))
                {
                    pushback_event(dir_monitor_event(boost::filesystem::path(dir) / itn->second.path().filename(), dir_monitor_event::modified));
                }
                old_entries.erase(ito);
            }
            else
            {
                pushback_event(dir_monitor_event(boost::filesystem::path(dir) / itn->second.path().filename(), dir_monitor_event::added));
            }
        }
        for (dir_entry_map::iterator it = old_entries.begin(); it != old_entries.end(); ++it)
        {
            pushback_event(dir_monitor_event(boost::filesystem::path(dir) / it->second.path().filename(), dir_monitor_event::removed));
        }
    }

    void work_thread()
    {
        while (running())
        {
            for (auto dir : dirs_)
            {
                struct timespec timeout;
                timeout.tv_sec = 0;
                timeout.tv_nsec = 200000000;
                unsigned eventFilter = NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB;
                struct kevent event;
                struct kevent eventData;
                EV_SET(&event, *dir->second, EVFILT_VNODE, EV_ADD | EV_CLEAR, eventFilter, 0, 0);
                int nEvents = kevent(kqueue_, &event, 1, &eventData, 1, &timeout);

                if (nEvents < 0 or eventData.flags == EV_ERROR)
                {
                    boost::system::system_error e(boost::system::error_code(errno, boost::system::get_system_category()), "boost::asio::dir_monitor_impl::work_thread: kevent failed");
                    boost::throw_exception(e);
                }

                // dir_monitor_event::event_type type = dir_monitor_event::null;
                // if (eventData.fflags & NOTE_WRITE) {
                //     type = dir_monitor_event::modified;
                // }
                // else if (eventData.fflags & NOTE_DELETE) {
                //     type = dir_monitor_event::removed;
                // }
                // else if (eventData.fflags & NOTE_RENAME) {
                //     type = dir_monitor_event::renamed_new_name;
                // case FILE_ACTION_RENAMED_OLD_NAME: type = dir_monitor_event::renamed_old_name; break;
                // case FILE_ACTION_RENAMED_NEW_NAME: type = dir_monitor_event::renamed_new_name; break;
                // }

                // Run recursive directory check to find changed files
                // Similar to Poco's DirectoryWatcher
                // @todo Use FSEvents API on OSX?

                dir_entry_map new_entries;
                scan(dir->first, new_entries);
                compare(dir->first, entries[dir->first], new_entries);
                std::swap(entries[dir->first], new_entries);
            }
        }
    }

    bool running()
    {
        // Access to run_ is sychronized with stop_work_thread().
        boost::mutex::scoped_lock lock(work_thread_mutex_);
        return run_;
    }

    void stop_work_thread()
    {
        // Access to run_ is sychronized with running().
        boost::mutex::scoped_lock lock(work_thread_mutex_);
        run_ = false;
    }

    int kqueue_;
    boost::unordered_map<std::string, dir_entry_map> entries;
    bool run_;
    boost::mutex work_thread_mutex_;
    boost::thread work_thread_;

    boost::mutex dirs_mutex_;
    boost::ptr_unordered_map<std::string, unix_handle> dirs_;

    boost::mutex events_mutex_;
    boost::condition_variable events_cond_;
    std::deque<dir_monitor_event> events_;
};

} // asio namespace
} // boost namespace

