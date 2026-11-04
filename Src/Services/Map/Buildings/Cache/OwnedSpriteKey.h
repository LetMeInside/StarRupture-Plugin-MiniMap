// Owns neutral sprite requirement bytes and maps compatible canonical keys to filenames.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Services::Map::Buildings::Cache
{
	/** Owns key bytes before asynchronous work can outlive a borrowed source publication.
	 * The cache treats the bytes as opaque identity and applies generic safe absolute-path
	 * validation needed for compatible reversible .sprite filenames, not asset-name rules.
	 */
	class OwnedSpriteKey final
	{
	public:
		/** Copies a borrowed canonical key after safe legacy-compatible validation.
		 * @param borrowedKey Key bytes borrowed from a source publication.
		 * @param output Receives an owning key on success.
		 * @return True when bytes satisfy the existing filename identity grammar.
		 */
		static bool TryCopy(std::string_view borrowedKey, OwnedSpriteKey& output)
		{
			if (!IsCanonicalKey(borrowedKey))
			{
				return false;
			}

			output.m_bytes.assign(borrowedKey);
			return true;
		}

		/** Returns the owned canonical key bytes without transferring ownership. */
		const std::string& Bytes() const noexcept { return m_bytes; }

		/** Reversibly encodes key bytes to the existing compatible .sprite basename.
		 * @param fileName Receives the encoded basename on success and is cleared on failure.
		 * @return True when the encoded basename fits the existing length limit.
		 */
		bool TryGetFileName(std::string& fileName) const
		{
			fileName.clear();
			if (!IsCanonicalKey(m_bytes))
			{
				return false;
			}

			EncodeBytes(fileName);
			fileName += ".sprite";
			if (fileName.size() <= 240)
			{
				return true;
			}

			fileName.clear();
			return false;
		}

	private:
		/** Checks UTF-8 and generic safe absolute-path syntax without interpreting key meaning. */
		static bool IsCanonicalKey(std::string_view key) noexcept
		{
			if (key.size() < 2 || key.size() > 4096 || key.front() != '/' || key.back() == '/' ||
				key.find('\\') != std::string_view::npos || key.find("//") != std::string_view::npos ||
				!IsValidUtf8(key))
			{
				return false;
			}

			for (unsigned char byte : key)
			{
				if (byte < 0x20 || byte == 0x7f)
				{
					return false;
				}
			}

			return HasValidComponents(key, key.size());
		}

		/** Rejects malformed, overlong, surrogate and out-of-range UTF-8 sequences.
		 * @param text Borrowed candidate key bytes.
		 * @return True only when every byte belongs to a valid UTF-8 sequence.
		 */
		static bool IsValidUtf8(std::string_view text) noexcept
		{
			unsigned remaining = 0;
			std::uint32_t codePoint = 0;
			std::uint32_t minimum = 0;
			for (unsigned char byte : text)
			{
				if (!ConsumeUtf8Byte(byte, remaining, codePoint, minimum)) return false;
			}
			return remaining == 0;
		}

		/** Advances UTF-8 decoder state for one byte or rejects that byte.
		 * @param byte Next byte from the key.
		 * @param remaining Continuation bytes still required by the current sequence.
		 * @param codePoint Accumulated code point for the current sequence.
		 * @param minimum Smallest allowed code point for the current sequence width.
		 * @return True when the byte is a valid next byte for the current decoder state.
		 */
		static bool ConsumeUtf8Byte(
			unsigned char byte,
			unsigned& remaining,
			std::uint32_t& codePoint,
			std::uint32_t& minimum) noexcept
		{
			if (!remaining)
			{
				return StartUtf8Sequence(byte, remaining, codePoint, minimum);
			}
			if ((byte & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (byte & 0x3f);
			--remaining;
			return remaining || IsValidCompletedCodePoint(codePoint, minimum);
		}

		/** Starts a valid UTF-8 sequence or accepts a single-byte ASCII character. */
		static bool StartUtf8Sequence(
			unsigned char byte,
			unsigned& remaining,
			std::uint32_t& codePoint,
			std::uint32_t& minimum) noexcept
		{
			if (byte <= 0x7f) return true;
			if (byte >= 0xc2 && byte <= 0xdf)
			{
				remaining = 1;
				codePoint = byte & 0x1f;
				minimum = 0x80;
				return true;
			}
			if (byte >= 0xe0 && byte <= 0xef)
			{
				remaining = 2;
				codePoint = byte & 0x0f;
				minimum = 0x800;
				return true;
			}
			if (byte >= 0xf0 && byte <= 0xf4)
			{
				remaining = 3;
				codePoint = byte & 0x07;
				minimum = 0x10000;
				return true;
			}
			return false;
		}

		/** Rejects overlong, surrogate and out-of-range completed UTF-8 code points. */
		static bool IsValidCompletedCodePoint(std::uint32_t codePoint, std::uint32_t minimum) noexcept
		{
			return codePoint >= minimum && codePoint <= 0x10ffff &&
				(codePoint < 0xd800 || codePoint > 0xdfff);
		}

		/** Validates that path components are nonempty and not dot segments. */
		static bool HasValidComponents(std::string_view key, std::size_t extension) noexcept
		{
			for (std::size_t start = 1; start < extension;)
			{
				const std::size_t separator = key.find('/', start);
				const std::size_t end = separator == std::string_view::npos || separator > extension
					? extension : separator;
				const std::string_view component = key.substr(start, end - start);
				if (component.empty() || component == "." || component == "..")
				{
					return false;
				}
				start = end + 1;
			}
			return true;
		}

		/** Percent-escapes every byte excluded by the existing filename encoder. */
		void EncodeBytes(std::string& fileName) const
		{
			constexpr char hex[] = "0123456789ABCDEF";
			for (unsigned char byte : m_bytes)
			{
				const bool safe = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
					(byte >= '0' && byte <= '9') || byte == '.' || byte == '-' || byte == '_';
				if (safe)
				{
					fileName.push_back(char(byte));
				}
				else
				{
					fileName.push_back('%');
					fileName.push_back(hex[byte >> 4]);
					fileName.push_back(hex[byte & 15]);
				}
			}
		}

		std::string m_bytes;
	};
}
