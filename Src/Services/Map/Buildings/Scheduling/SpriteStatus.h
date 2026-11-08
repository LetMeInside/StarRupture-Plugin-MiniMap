// Represents scheduler-owned acquisition state for one canonical Blueprint sprite key.
#pragma once

#include "../Capture/CaptureRequest.h"

namespace Services::Map::Buildings::Scheduling
{
	/** Stores one key's acquisition state and pending correlation, never image or capture resources.
	 * Absence from BuildingScheduler's current-session map represents an unknown requirement.
	 */
	class SpriteStatus final
	{
	public:
		/** States needed to suppress duplicate work and report sprite readiness. */
		enum class State
		{
			CapturePending,
			Ready,
			TerminalFailure,
			TemporarilyUnavailable
		};

		/** Creates one status value in the supplied acquisition state.
		 * @param state Initial per-key state; callers use absence for Unknown.
		 */
		explicit SpriteStatus(State state) noexcept
			: m_state(state)
		{
		}

		/** Returns the acquisition state without exposing or owning a CPU image. */
		State GetState() const noexcept
		{
			return m_state;
		}

		/** Returns the originating identity for pending work, or zeroed identity otherwise. */
		Capture::CaptureRequest::Correlation GetRequestIdentity() const noexcept
		{
			return m_requestIdentity;
		}

		/** Records a queued request and its correlation for later terminal-result matching.
		 * @param requestIdentity Sequence identifying this individual request.
		 */
		void MarkPending(Capture::CaptureRequest::Correlation requestIdentity) noexcept
		{
			m_state = State::CapturePending;
			m_requestIdentity = requestIdentity;
		}

		/** Marks a current CPU or disk payload ready and removes any pending correlation. */
		void MarkReady() noexcept
		{
			m_state = State::Ready;
			m_requestIdentity = {};
		}

		/** Retains a terminal failure only for the exact request still pending for this key.
		 * Call only after matching terminal failure feedback has been received.
		 * @param requestIdentity Request sequence returned by terminal feedback.
		 * @return True when the matching pending request became terminally failed.
		 */
		bool MarkTerminalFailure(Capture::CaptureRequest::Correlation requestIdentity) noexcept
		{
			if (!IsPendingFor(requestIdentity))
			{
				return false;
			}

			m_state = State::TerminalFailure;
			m_requestIdentity = {};
			return true;
		}

		/** Marks an uncertain cache/source outcome as temporary and removes pending identity. */
		void MarkTemporarilyUnavailable() noexcept
		{
			m_state = State::TemporarilyUnavailable;
			m_requestIdentity = {};
		}

		/** Checks that a result belongs to this key's currently pending request.
		 * @param requestIdentity Request sequence returned by terminal feedback.
		 * @return True only for an exact pending correlation match.
		 */
		bool IsPendingFor(Capture::CaptureRequest::Correlation requestIdentity) const noexcept
		{
			return m_state == State::CapturePending &&
				m_requestIdentity.RequestSequence == requestIdentity.RequestSequence;
		}

	private:
		State m_state;
		Capture::CaptureRequest::Correlation m_requestIdentity;
	};
}
