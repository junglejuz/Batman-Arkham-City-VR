# Third-party notices

Batman City VR is built on other people's work. Thank you.

| Project | Licence | Source | Used for |
|---|---|---|---|
| bioshock-vr | MIT (see `LICENSE-bioshock-vr.txt`) | https://github.com/mohamad-balouza/bioshock-vr | the mod's core: OpenXR, hooks, HUD capture, the in-headset menu; its BioShock Infinite (Unreal Engine 3) stereo method |
| MinHook | BSD-2-Clause | https://github.com/TsudaKageyu/minhook | function hooking |
| Dear ImGui | MIT | https://github.com/ocornut/imgui | the in-headset VR menu |
| OpenXR SDK (loader) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK | talking to the PC VR software |
| Luma Framework (Filippo Tarpini, "Garamond") | custom MIT, credit required (see `LICENSE-luma-framework.md`) | https://github.com/Filoppi/Luma-Framework | the method for per-eye DLSS (jitter, camera motion vectors, sharper textures), first worked out in Luma's Arkham Knight mod |
| NVIDIA DLSS (`BatmanCityDLSS\nvngx_dlss.dll`) | NVIDIA RTX SDK licence; © NVIDIA Corporation | https://developer.nvidia.com/rtx/dlss | upscaling and anti-aliasing (optional, NVIDIA RTX graphics cards) |

The higher-resolution loading screens and logo pictures in `BatmanCityData\hd\` were made from the game's own pictures
(Batman: Arkham City © Warner Bros. Entertainment Inc.; Batman and related characters are trademarks of DC Comics).
They are only for use with your own copy of the game.
