#pragma once

#include "Services/Log.h"

namespace MiniMap::Adapters::AlienX
{
    class Log final : public Services::ILog
    {
    public:
        void Write(Services::Level level, std::string_view utf8) noexcept override;
    };
}
