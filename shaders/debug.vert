#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 fragColor;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 directional_direction;
    vec4 directional_color;
    vec4 ambient_light;
    uvec4 light_counts;
} scene;

void main() {
    gl_Position = scene.view_projection * vec4(inPos, 1.0);
    fragColor = inColor;
}