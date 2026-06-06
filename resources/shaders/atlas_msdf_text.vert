#version 440

layout(location = 0) in vec2 vertex_position;
layout(location = 1) in vec4 instance_rect;
layout(location = 2) in vec4 instance_uv_rect;
layout(location = 3) in vec4 instance_color;
layout(location = 4) in vec4 instance_uv_bounds;
layout(location = 5) in vec4 instance_frame_rect;

layout(std140, binding = 0) uniform msdf_text_block
{
    mat4  mvp;
    float px_range;
    float target_width;
    float target_height;
    float reserved0;
};

layout(location = 0) smooth out vec4 fragment_uv_rect;
layout(location = 1) smooth out vec4 fragment_color;
layout(location = 2) smooth out vec4 fragment_uv_bounds;
layout(location = 3) smooth out vec4 fragment_frame_rect;

vec2 frame_position(vec2 local_position)
{
    vec4 clip = mvp * vec4(local_position, 0.0, 1.0);
    vec2 ndc = clip.xy / clip.w;
    return vec2(
        (ndc.x * 0.5 + 0.5) * target_width,
        (0.5 - ndc.y * 0.5) * target_height);
}

void main()
{
    vec2 local_position = instance_rect.xy + vertex_position * instance_rect.zw;
    gl_Position = mvp * vec4(local_position, 0.0, 1.0);
    vec2 frame_a = frame_position(instance_rect.xy);
    vec2 frame_b = frame_position(instance_rect.xy + instance_rect.zw);
    vec2 frame_min = min(frame_a, frame_b);
    vec2 frame_max = max(frame_a, frame_b);
    vec2 transform_delta =
        round(frame_min - instance_frame_rect.xy);
    fragment_uv_rect = instance_uv_rect;
    fragment_color = instance_color;
    fragment_uv_bounds = instance_uv_bounds;
    fragment_frame_rect = vec4(
        instance_frame_rect.xy + transform_delta,
        frame_max - frame_min);
}
