// Owns the game-independent facts needed to decide whether a sprite subject can
// be rendered in the independent private capture world.
#pragma once

#include "../Cache/OwnedSpriteKey.h"

#include <string>
#include <vector>

namespace Services::Map::Buildings::Capture
{
	/** Identifies one eligible actor class and its opaque sprite key without game policy.
	 * The host owns class interpretation and provides these copied facts; no universal
	 * entity identifier or borrowed Unreal object is retained by the request.
	 */
	struct CaptureSubjectFacts
	{
		/** Describes copied game-world dependencies for this subject; owns no engine objects. */
		struct IsolationFacts
		{
			/** Object paths of required world subsystems, copied from host classification. */
			std::vector<std::string> RequiredWorldSubsystemClassPaths;
			/** Host's explicit result after checking the isolated-world requirements above. */
			bool IsCompatibleWithPrivateCaptureWorld = false;
			/** True when rendering depends on state owned by the active gameplay world. */
			bool RequiresGameplayWorldState = false;
			/** True when the subject must tick in the active gameplay world to render correctly. */
			bool RequiresGameplayWorldTick = false;
			/** True when the subject refers to actors or components outside its private world. */
			bool ReferencesGameplayWorldObjects = false;
		};

		/** Existing opaque sprite identity used by cache, scheduler, and CPU source manager. */
		Cache::OwnedSpriteKey SpriteRequirement;
		/** Stable Unreal class object path supplied by the host for capture preparation. */
		std::string ActorClassObjectPath;
		/** Host-established dependencies used to reject subjects unsafe in isolation. */
		IsolationFacts Isolation;
	};
}
