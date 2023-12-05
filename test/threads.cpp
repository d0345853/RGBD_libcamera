/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * threads.cpp - Threads test
 */

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include <libcamera/base/thread.h>

#include "test.h"

using namespace std;
using namespace libcamera;

class DelayThread : public Thread
{
public:
	DelayThread(chrono::steady_clock::duration duration)
		: duration_(duration)
	{
	}

protected:
	void run()
	{
		this_thread::sleep_for(duration_);
	}

private:
	chrono::steady_clock::duration duration_;
};

class ThreadTest : public Test
{
protected:
	int init()
	{
		return 0;
	}

	int run()
	{
		/* Test Thread() retrieval for the main thread. */
		Thread *mainThread = Thread::current();
		if (!mainThread) {
			cout << "Thread::current() failed to main thread"
			     << endl;
			return TestFail;
		}

		if (!mainThread->isRunning()) {
			cout << "Main thread is not running" << endl;
			return TestFail;
		}

		/* Test starting the main thread, the test shall not crash. */
		mainThread->start();

		/* Test the running state of a custom thread. */
		std::unique_ptr<Thread> thread = std::make_unique<Thread>();
		thread->start();

		if (!thread->isRunning()) {
			cout << "Thread is not running after being started"
			     << endl;
			return TestFail;
		}

		thread->exit(0);
		thread->wait();

		if (thread->isRunning()) {
			cout << "Thread is still running after finishing"
			     << endl;
			return TestFail;
		}

		/* Test waiting for completion with a timeout. */
		thread = std::make_unique<DelayThread>(chrono::milliseconds(500));
		thread->start();
		thread->exit(0);

		bool timeout = !thread->wait(chrono::milliseconds(100));

		if (!timeout) {
			cout << "Waiting for thread didn't time out" << endl;
			return TestFail;
		}

		timeout = !thread->wait(chrono::milliseconds(1000));

		if (timeout) {
			cout << "Waiting for thread timed out" << endl;
			return TestFail;
		}

		/* Test waiting on a thread that isn't running. */
		thread = std::make_unique<Thread>();

		timeout = !thread->wait();
		if (timeout) {
			cout << "Waiting for non-started thread timed out" << endl;
			return TestFail;
		}

		thread->start();
		thread->exit(0);
		thread->wait();

		timeout = !thread->wait();
		if (timeout) {
			cout << "Waiting for already stopped thread timed out" << endl;
			return TestFail;
		}

		return TestPass;
	}

	void cleanup()
	{
	}
};

TEST_REGISTER(ThreadTest)
