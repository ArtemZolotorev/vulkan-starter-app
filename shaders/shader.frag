#version 450

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragColor;

layout(location = 0) out vec4 outColor;

struct PointLight {
    vec4 position;
    vec4 color;
};

struct SpotLight {
    vec4 position;
    vec4 color;
    vec4 direction;
    vec4 params;
};

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 directional_direction;
    vec4 directional_color;
    vec4 ambient_light;
    uvec4 light_counts;
} scene;

// ВАЖНО: добавлено слово readonly
layout(set = 0, binding = 1) readonly buffer PointLights {
    PointLight point_lights[];
};

// ВАЖНО: добавлено слово readonly
layout(set = 0, binding = 2) readonly buffer SpotLights {
    SpotLight spot_lights[];
};

layout(set = 1, binding = 0) uniform ModelUniforms {
    mat4 model;
    mat4 normal_matrix;
    vec4 albedo;
    vec4 specular;
} mdl;

vec3 computeBlinnPhong(vec3 N, vec3 V, vec3 L, vec3 lightColor, vec3 albedo, vec3 spec, float shininess) {
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = albedo * NdotL;

    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    vec3 specularTerm = spec * pow(NdotH, shininess);

    return lightColor * (diffuse + specularTerm);
}

void main() {
    vec3 N = normalize(fragNormal);
    vec3 V = normalize(scene.camera_position.xyz - fragWorldPos);
    vec3 albedo = mdl.albedo.rgb * fragColor;
    vec3 spec = mdl.specular.rgb;
    float shininess = max(mdl.specular.w, 1.0);

    // Ambient
    vec3 color = scene.ambient_light.rgb * albedo;

    // Directional Light
    {
        vec3 L = normalize(-scene.directional_direction.xyz);
        color += computeBlinnPhong(N, V, L, scene.directional_color.rgb * scene.directional_color.a, albedo, spec, shininess);
    }

    // Point Lights
    uint point_count = scene.light_counts.x;
    for (uint i = 0; i < point_count; ++i) {
        vec3 toLight = point_lights[i].position.xyz - fragWorldPos;
        float dist = length(toLight);
        vec3 L = toLight / max(dist, 1e-4); // Защита от деления на 0
        
        // Затухание по закону обратных квадратов (1 / d^2)
        float attenuation = 1.0 / (1.0 + dist * dist);
        
        vec3 lightCol = point_lights[i].color.rgb * point_lights[i].color.a * attenuation;
        color += computeBlinnPhong(N, V, L, lightCol, albedo, spec, shininess);
    }

    // Spot Lights
    uint spot_count = scene.light_counts.y;
    for (uint i = 0; i < spot_count; ++i) {
        vec3 toLight = spot_lights[i].position.xyz - fragWorldPos;
        float dist = length(toLight);
        vec3 L = toLight / max(dist, 1e-4);
        
        vec3 spotDir = normalize(-spot_lights[i].direction.xyz);
        float cosAngle = dot(L, spotDir);
        
        float innerCos = cos(radians(spot_lights[i].params.x));
        float outerCos = cos(radians(spot_lights[i].params.y));
        
        // Мягкие края (smoothstep)
        float spotFactor = smoothstep(outerCos, innerCos, cosAngle);

        if (spotFactor > 0.0) {
            float attenuation = 1.0 / (1.0 + dist * dist);
            vec3 lightCol = spot_lights[i].color.rgb * spot_lights[i].color.a * attenuation * spotFactor;
            color += computeBlinnPhong(N, V, L, lightCol, albedo, spec, shininess);
        }
    }

    outColor = vec4(color, 1.0);
}