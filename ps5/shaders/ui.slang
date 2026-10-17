#version 450
// Genesis Plus GX PS5: the menus, the shelf and the HUD (the CPU-drawn 1920x1080 surface, premultiplied alpha)
// over the game picture. Pixel-exact: each output pixel reads its surface pixel, so at 4K every surface pixel
// becomes a 2x2 block, as sharp as at 1080p.
// SPDX-License-Identifier: MIT

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
} params;

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
	gl_Position = global.MVP * Position;
	vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
	ivec2 p = ivec2(min(vTexCoord * params.SourceSize.xy, params.SourceSize.xy - 1.0));
	FragColor = texelFetch(Source, p, 0);
}
