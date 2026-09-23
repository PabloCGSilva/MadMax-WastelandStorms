# Third-party code

This repository bundles the following sources, each under its own license.

## MinHook — `minhook/`

The Minimalistic x86/x64 API Hooking Library for Windows.
Copyright (C) 2009-2017 Tsuda Kageyu. All rights reserved.
BSD 2-Clause License — https://github.com/TsudaKageyu/minhook/blob/master/LICENSE.txt

Includes Hacker Disassembler Engine 32/64 C (HDE32/HDE64),
Copyright (c) 2008-2009, Vyacheslav Patkov. BSD-style license.

## Dear ImGui 1.87 — `imgui/`

Copyright (c) 2014-2022 Omar Cornut.
MIT License — https://github.com/ocornut/imgui/blob/master/LICENSE.txt

`imgui.cpp` is taken from the copy vendored in gigaHours' mm_sdk, where
`ImGui::IsKeyDown` had been commented out; it is restored here to the upstream
definition.

## Ultimate ASI Loader — `dinput8.dll` (release package only)

By ThirteenAG — https://github.com/ThirteenAG/Ultimate-ASI-Loader (MIT).

## Game data

No Mad Max file is included. The UI font is read at runtime from the player's
own installed archives.
