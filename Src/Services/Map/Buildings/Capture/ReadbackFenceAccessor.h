// Provides member-pointer access to the protected UE GPU readback fence.
#pragma once

#include "Generated/Unreal/RHIGPUReadbackCompat.h"

namespace Services::Map::Buildings::Capture
{
	/** Reads the genuine base readback's protected fence without adding fields or constructing an object. */
	class ReadbackFenceAccessor final : public FRHIGPUMemoryReadback
	{
	public:
		/** Returns the base readback's existing fence reference for native completion checks. */
		static FGPUFenceRHIRef& GetFenceReference(FRHIGPUMemoryReadback& readback)
		{
			static constexpr auto fenceMember = &ReadbackFenceAccessor::Fence;
			return readback.*fenceMember;
		}

	private:
		/** Prevents accessor instances; the type exists only to name the inherited protected member. */
		ReadbackFenceAccessor() = delete;
	};
}
