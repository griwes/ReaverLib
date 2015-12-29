/**
 * Reaver Library License
 *
 * Copyright © 2013-2014 Michał "Griwes" Dominiak
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation is required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 **/

#pragma once

#include <thread>
#include <functional>
#include <unordered_map>
#include <map>
#include <queue>
#include <atomic>
#include <mutex>
#include <future>
#include <type_traits>

#include "exception.h"
#include "callbacks.h"
#include "thread.h"
#include "semaphore.h"

namespace reaver { inline namespace _v1
{
    class thread_pool_closed : public exception
    {
    public:
        thread_pool_closed() : exception{ logger::crash }
        {
            *this << "tried to insert a task into an already closed thread pool.";
        }
    };

    class free_affinities_exhausted : public exception
    {
    public:
        free_affinities_exhausted() : exception{ logger::crash }
        {
            *this << "free affinities in a thread pool exhausted.";
        }
    };

    class invalid_affinity : public exception
    {
    public:
        invalid_affinity() : exception{ logger::crash }
        {
            *this << "invalid affinity passed to thread pool push().";
        }
    };

    class thread_pool
    {
    public:
        thread_pool(std::size_t size)
        {
            while (size--)
            {
                _spawn();
            }
        }

        ~thread_pool()
        {
            _end = true;
            _cond.notify_all();

            for (auto & th : _threads)
            {
                try
                {
                    if (th.second.joinable())
                    {
                        th.second.join();
                    }
                }

                catch (...)
                {
                }
            }
        }

        void abort()
        {
            {
                std::unique_lock<std::mutex> lock{ _lock };

                _queue = decltype(_queue){};
                _affinity_queues = {};
                _end = true;
                _cond.notify_all();
            }

            for (auto & th : _threads)
            {
                try
                {
                    th.second.join();
                }

                catch (...)
                {
                }
            }
        }

        template<typename F, typename... Args>
        std::future<typename std::result_of<F (Args...)>::type> push(F && f, Args &&... args)
        {
            auto task = std::make_shared<std::packaged_task<typename std::result_of<F (Args...)>::type ()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            auto future = task->get_future();

            {
                std::unique_lock<std::mutex> lock{ _lock };

                if (_end)
                {
                    throw thread_pool_closed{};
                }

                _queue.emplace([task]{ (*task)(); });
            }

            _cond.notify_one();

            return future;
        }

        template<typename F, typename... Args>
        std::future<typename std::result_of<F (Args...)>::type> push(std::thread::id affinity, F && f, Args &&... args)
        {
            if (affinity == std::thread::id{})
            {
                return push(std::forward<F>(f), std::forward<Args>(args)...);
            }

            auto task = std::make_shared<std::packaged_task<typename std::result_of<F (Args...)>::type ()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            auto future = task->get_future();

            {
                std::unique_lock<std::mutex> lock{ _lock };

                if (_end)
                {
                    throw thread_pool_closed{};
                }

                if (!_affinity_queues.count(affinity))
                {
                    throw invalid_affinity{};
                }

                _affinity_queues[affinity].emplace([task]{ (*task)(); });
            }

            _cond.notify_all();

            return future;
        }

        template<typename Container, typename F, typename... Args>
        std::future<typename std::enable_if<std::is_same<typename Container::value_type, std::thread::id>::value,
            typename std::result_of<F (Args...)>::type>::type> push(const Container & affinities, F && f, Args &&... args)
        {
            std::size_t smallest_size = -1;
            std::thread::id smallest_id;

            for (const auto & aff : affinities)
            {
                uint64_t current_size = 0;

                {
                    std::unique_lock<std::mutex> lock{ _lock };
                    current_size = _affinity_queues[aff].size();
                }

                if (current_size < smallest_size)
                {
                    smallest_size = _affinity_queues[aff].size();
                    smallest_id = aff;
                }
            }

            if (smallest_id == std::thread::id{})
            {
                return push(std::forward<F>(f), std::forward<Args>(args)...);
            }

            return push(smallest_id, std::forward<F>(f), std::forward<Args>(args)...);
        }

        std::size_t size() const
        {
            return _size;
        }

        void resize(std::size_t new_size)
        {
            std::unique_lock<std::mutex> lock{ _lock };

            if (new_size == _size)
            {
                return;
            }

            if (new_size > _size)
            {
                while (_size < new_size)
                {
                    _spawn();
                }

                return;
            }

            _die_semaphore.notify(_size - new_size);
            _cond.notify_all();
            _size = new_size;
        }

        std::thread::id allocate_affinity(bool insert = false)
        {
            std::unique_lock<std::mutex> lock{ _lock };

            if (_affinities.empty() && insert)
            {
                _spawn();
            }

            if (_affinities.size())
            {
                auto ret = _affinities.back();
                _affinities.pop_back();
                return ret;
            }

            throw free_affinities_exhausted{};
        }

    private:
        void _loop()
        {
            if (_waiters)
            {
                std::unique_lock<std::mutex> lock{ _lock };
                _waiters();
            }

            while (!_end || _threads.size())
            {
                {
                    std::unique_lock<std::mutex> lock{ _lock };

                    if (_try_die())
                    {
                        return;
                    }
                }

                std::function<void ()> f;

                {
                    auto & this_queue = _affinity_queues[std::this_thread::get_id()];

                    std::unique_lock<std::mutex> lock{ _lock };

                    while (!_end && this_queue.empty() && _queue.empty())
                    {
                        if (_try_die())
                        {
                            return;
                        }

                        _cond.wait(lock);
                    }

                    if (_end && this_queue.empty() && _queue.empty())
                    {
                        return;
                    }

                    if (this_queue.size())
                    {
                        f = std::move(this_queue.front());
                        this_queue.pop();
                    }

                    else if (_queue.size())
                    {
                        f = std::move(_queue.front());
                        _queue.pop();
                    }

                    else
                    {
                        continue;
                    }
                }

                f();

                if (_waiters)
                {
                    std::unique_lock<std::mutex> lock{ _lock };
                    _waiters();
                }
            }
        }

        bool _try_die()
        {
            if (_die_semaphore.try_wait())
            {
                auto this_thread_id = std::this_thread::get_id();

                if (!_affinity_queues[this_thread_id].empty())
                {
                    _die_semaphore.notify();
                    return false;
                }

                _affinities.erase(std::find(_affinities.begin(), _affinities.end(), this_thread_id));
                _affinity_queues.erase(_affinity_queues.find(this_thread_id));

                _threads.erase(_threads.find(this_thread_id));
                --_size;

                return true;
            }

            return false;
        }

        void _spawn()
        {
            detaching_thread t{ &thread_pool::_loop, this };
            auto id = t.get_id();
            _threads[id] = std::move(t);
            _affinities.push_back(id);
            _affinity_queues[id];
            ++_size;
        }

        std::atomic<std::size_t> _size{ 0 };

        std::map<std::thread::id, detaching_thread> _threads;
        std::unordered_map<std::thread::id, std::queue<std::function<void ()>>> _affinity_queues;
        std::queue<std::function<void ()>> _queue;

        std::vector<std::thread::id> _affinities;

        std::condition_variable _cond;
        std::mutex _lock;

        semaphore _die_semaphore;

        std::atomic<bool> _end{ false };

        callbacks<void (void)> _waiters;
    };
}}
