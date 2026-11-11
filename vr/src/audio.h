// Stream audio: Opus (from moonlight-common-c) -> SDL audio output.
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <Limelight.h>

// Callbacks for LiStartConnection. SDL's audio subsystem must be initialised.
AUDIO_RENDERER_CALLBACKS audio_callbacks();
