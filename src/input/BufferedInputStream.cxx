/*
 * Copyright 2003-2018 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "BufferedInputStream.hxx"
#include "thread/Cond.hxx"
#include "thread/Name.hxx"

#include <string.h>

BufferedInputStream::BufferedInputStream(InputStreamPtr _input)
	:InputStream(_input->GetURI(), _input->mutex),
	 input(std::move(_input)),
	 thread(BIND_THIS_METHOD(RunThread)),
	 buffer(input->GetSize())
{
	assert(IsEligible(*input));

	input->SetHandler(this);

	if (input->HasMimeType())
		SetMimeType(input->GetMimeType());

	size = input->GetSize();
	seekable = input->IsSeekable();
	offset = input->GetOffset();

	SetReady();

	thread.Start();
}

BufferedInputStream::~BufferedInputStream() noexcept
{
	{
		const std::lock_guard<Mutex> lock(mutex);
		stop = true;
		wake_cond.notify_one();
	}

	thread.Join();
}

void
BufferedInputStream::Check()
{
	if (input)
		input->Check();
}

void
BufferedInputStream::Seek(std::unique_lock<Mutex> &lock,
			  offset_type new_offset)
{
	if (new_offset >= size) {
		offset = size;
		return;
	}

	auto r = buffer.Read(new_offset);
	if (r.HasData()) {
		/* nice, we already have some data at the desired
		   offset and this method call is a no-op */
		offset = new_offset;
		return;
	}

	seek_offset = new_offset;
	seek = true;
	wake_cond.notify_one();

	client_cond.wait(lock, [this]{ return !seek; });

	if (seek_error)
		std::rethrow_exception(std::exchange(seek_error, {}));

	offset = new_offset;
}

bool
BufferedInputStream::IsEOF() noexcept
{
	return offset == size;
}

bool
BufferedInputStream::IsAvailable() noexcept
{
	return IsEOF() || buffer.Read(offset).HasData();
}

size_t
BufferedInputStream::Read(std::unique_lock<Mutex> &lock,
			  void *ptr, size_t s)
{
	if (offset >= size)
		return 0;

	while (true) {
		assert(size == buffer.size());

		auto r = buffer.Read(offset);
		if (r.HasData()) {
			/* yay, we have some data */
			size_t nbytes = std::min(s, r.defined_buffer.size);
			memcpy(ptr, r.defined_buffer.data, nbytes);
			offset += nbytes;

			if (!IsAvailable()) {
				/* wake up the sleeping thread */
				idle = false;
				wake_cond.notify_one();
			}

			return nbytes;
		}

		if (read_error) {
			wake_cond.notify_one();
			std::rethrow_exception(std::exchange(read_error, {}));
		}

		if (idle) {
			/* wake up the sleeping thread */
			idle = false;
			wake_cond.notify_one();
		}

		client_cond.wait(lock);
	}
}

void
BufferedInputStream::RunThread() noexcept
{
	SetThreadName("input_buffered");

	std::unique_lock<Mutex> lock(mutex);

	while (!stop) {
		assert(size == buffer.size());

		if (seek) {
			try {
				input->Seek(lock, seek_offset);
			} catch (...) {
				seek_error = std::current_exception();
			}

			idle = false;
			seek = false;
			client_cond.notify_one();
		} else if (!idle && !read_error &&
			   offset != input->GetOffset() &&
			   !IsAvailable()) {
			/* a past Seek() call was a no-op because data
			   was already available at that position, but
			   now we've reached a new position where
			   there is no more data in the buffer, and
			   our input is reading somewhere else (maybe
			   stuck at the end of the file); to find a
			   way out, we now seek our input to our
			   reading position to be able to fill our
			   buffer */

			try {
				input->Seek(lock, offset);
			} catch (...) {
				/* this is really a seek error, but we
				   register it as a read_error,
				   because seek_error is only checked
				   by Seek(), and at our frontend (our
				   own InputStream interface) is in
				   "read" mode */
				read_error = std::current_exception();
			}
		} else if (!idle && !read_error &&
			   input->IsAvailable() && !input->IsEOF()) {
			const auto read_offset = input->GetOffset();
			auto w = buffer.Write(read_offset);

			if (w.empty()) {
				if (IsAvailable()) {
					/* we still have enough data
					   for the next Read() - sleep
					   until we need more data */
					idle = true;
				} else {
					/* we need more data at our
					   current position, because
					   the next Read() will stall
					   - seek our input to our
					   offset to prepare filling
					   the buffer from there */
					try {
						input->Seek(lock, offset);
					} catch (...) {
						read_error = std::current_exception();
						client_cond.notify_one();
						InvokeOnAvailable();
					}
				}

				continue;
			}

			try {
				size_t nbytes = input->Read(lock,
							    w.data, w.size);
				buffer.Commit(read_offset,
					      read_offset + nbytes);
			} catch (...) {
				read_error = std::current_exception();
			}

			client_cond.notify_one();
			InvokeOnAvailable();
		} else
			wake_cond.wait(lock);
	}
}
