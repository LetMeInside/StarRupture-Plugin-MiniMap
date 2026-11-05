// Provides the independently synchronized FIFO handoff for owned terminal capture feedback.
#pragma once

#include "../Capture/CaptureFeedback.h"

#include <deque>
#include <mutex>
#include <utility>

namespace Services::Map::Buildings::Scheduling
{
	/** Owns queued terminal feedback values and synchronizes only this feedback FIFO. */
	class SpriteFeedbackQueue final
	{
	public:
		/** Moves one terminal result into the FIFO without copying its immutable pixel allocation.
		 * @param feedback Feedback whose key, correlation, and shared payload ownership transfer in.
		 */
		void Enqueue(Capture::CaptureFeedback&& feedback)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_items.push_back(std::move(feedback));
		}

		/** Moves the oldest terminal result into caller-owned output storage.
		 * @param feedback Receives the oldest feedback when one is available.
		 * @return True when feedback was removed; false when the FIFO was empty.
		 */
		bool TryDequeue(Capture::CaptureFeedback& feedback)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_items.empty())
			{
				return false;
			}

			feedback = std::move(m_items.front());
			m_items.pop_front();
			return true;
		}

		/** Releases all queued feedback and its shared payload references; the FIFO remains reusable. */
		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_items.clear();
		}

	private:
		std::mutex m_mutex;
		std::deque<Capture::CaptureFeedback> m_items;
	};
}
