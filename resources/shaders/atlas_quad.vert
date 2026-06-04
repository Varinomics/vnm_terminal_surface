#version 440

layout(location = 0) in vec2 vertex_position;
layout(location = 1) in vec4 instance_rect;
layout(location = 2) in vec4 instance_color;

layout(std140, binding = 0) uniform matrix_block
{
    mat4 mvp;
};

layout(location = 0) out vec4 fragment_color;

void main()
{
    vec2 local_position = instance_rect.xy + vertex_position * instance_rect.zw;
    gl_Position = mvp * vec4(local_position, 0.0, 1.0);
    fragment_color = instance_color;
}
