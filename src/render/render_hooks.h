// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of PESMod.
//
// PESMod is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// PESMod is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with PESMod.  If not, see <https://www.gnu.org/licenses/>.

// render_hooks.h
#pragma once

namespace RenderHooks
{
    // Installs the D3D8 interception layer. A no-op unless [render] enabled=1
    // in PESMod.ini, so an install that has not opted in is untouched.
    void Register();

    // Writes the session report if one is pending. Hook removal itself is
    // handled by MinHook's global teardown in ModShutdown.
    void Unregister();
}
