/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Replaces TrinityCore's stock stub, which is an empty AddCustomScripts().
//
// Without this file the bot scripts COMPILE and are never registered: the core
// only ever calls AddCustomScripts(), and a fresh checkout's version calls
// nothing at all. The symptom is the worst kind — a clean build, a server that
// starts, and .pbot simply not existing.

// This is where scripts' loading functions should be declared:

void AddBotsScripts();

// The name of this function should match:
// void Add${NameOfDirectory}Scripts()
void AddCustomScripts()
{
    AddBotsScripts();
}
