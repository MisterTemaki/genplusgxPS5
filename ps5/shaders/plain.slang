#version 450
// Genesis Plus GX PS5: the plain picture on the GPU (shader "Off"): the game's frame scaled with the nearest or
// the bilinear filter (the pass's sampler), and the "Scanlines" option: the last screen row of every console line
// at half brightness, when each line gets 3 rows or more -- as the CPU scaler does it.
// SPDX-License-Identifier: MIT

layout(push_constant) uniform Push
{
	vec4 SourceSize;
	vec4 OutputSize;
	float SCANLINES;
} params;

#pragma parameter SCANLINES "Scanlines" 0.0 0.0 1.0 1.0

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
	vec4 c = texture(Source, vTexCoord);
	float rows = params.OutputSize.y * params.SourceSize.w;
	if (params.SCANLINES > 0.5 && rows >= 3.0)
	{
		float r = floor(vTexCoord.y * params.OutputSize.y);
		float line = floor((r + 0.5) / rows);
		if (r + 1.0 >= floor((line + 1.0) * rows))
			c.rgb *= 0.5;
	}
	FragColor = vec4(c.rgb, 1.0);
}
