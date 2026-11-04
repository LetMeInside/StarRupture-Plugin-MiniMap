// Owns the COM apartment initialization count required by SpritePngCodec operations.
#pragma once

#include <objbase.h>

namespace Services::Map::Buildings::Cache
{
	/** Balances COM initialization for one WIC codec operation on the calling thread. */
	class SpritePngCodecApartment final
	{
	public:
		/** Initializes multithreaded COM or observes an already selected apartment model. */
		SpritePngCodecApartment() noexcept
			: m_initializationResult(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
			  m_ownsInitialization(SUCCEEDED(m_initializationResult))
		{
		}

		/** Releases only the COM initialization count acquired by this instance. */
		~SpritePngCodecApartment()
		{
			if (m_ownsInitialization)
			{
				CoUninitialize();
			}
		}

		/** Returns whether WIC can run on the current COM apartment. */
		bool IsAvailable() const noexcept
		{
			return SUCCEEDED(m_initializationResult) || m_initializationResult == RPC_E_CHANGED_MODE;
		}

		/** Returns the HRESULT from COM initialization when the apartment is unavailable. */
		HRESULT InitializationError() const noexcept
		{
			return m_initializationResult;
		}

	private:
		HRESULT m_initializationResult;
		bool m_ownsInitialization;
	};
}
