// Provides the independently synchronized FIFO handoff for owned sprite capture requests.
#pragma once

#include "../Capture/CaptureRequest.h"

#include <deque>
#include <mutex>
#include <utility>

namespace Services::Map::Buildings::Scheduling
{
	/** Owns queued request values and synchronizes only operations on this request FIFO. */
	class SpriteRequestQueue final
	{
	public:
		/** Moves one caller-owned request into the FIFO.
		 * @param request Request whose key and correlation ownership transfers into the queue.
		 */
		void Enqueue(Capture::CaptureRequest&& request)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_items.push_back(std::move(request));
		}

		/** Moves the oldest queued request into caller-owned output storage.
		 * @param request Receives the oldest request when one is available.
		 * @return True when a request was removed; false when the FIFO was empty.
		 */
		bool TryDequeue(Capture::CaptureRequest& request)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_items.empty())
			{
				return false;
			}

			request = std::move(m_items.front());
			m_items.pop_front();
			return true;
		}

		/** Releases all queued request values while leaving the FIFO available for reuse. */
		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_items.clear();
		}

	private:
		std::mutex m_mutex;
		std::deque<Capture::CaptureRequest> m_items;
	};
}
