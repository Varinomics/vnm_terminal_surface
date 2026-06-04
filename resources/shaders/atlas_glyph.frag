#version 440

layout(location = 0) in vec2 fragment_uv;
layout(location = 1) in vec4 fragment_color;

layout(binding = 1) uniform sampler2D coverage_atlas;

layout(location = 0) out vec4 output_color;

void main()
{
    float coverage = texture(coverage_atlas, fragment_uv).r;
    output_color = vec4(fragment_color.rgb * coverage, fragment_color.a * coverage);
}
