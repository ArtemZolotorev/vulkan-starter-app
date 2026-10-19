#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "application.hpp"

#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/constants.hpp>

#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <fstream>
#include <array>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace application {

	// ==== Структуры вершин ====
	struct Vertex {
		glm::vec3 pos;
		glm::vec3 normal;
		glm::vec3 color;
	};

	struct DebugVertex {
		glm::vec3 pos;
		glm::vec3 color;
	};

	// ==== Данные для GPU (совпадают с шейдерами) ====
	struct PointLightGPU {
		glm::vec4 position;
		glm::vec4 color;
	};

	struct SpotLightGPU {
		glm::vec4 position;
		glm::vec4 color;
		glm::vec4 direction;
		glm::vec4 params; // x = innerAngle, y = outerAngle
	};

	struct SceneUniforms {
		glm::mat4 view_projection;
		glm::vec4 camera_position;
		glm::vec4 directional_direction;
		glm::vec4 directional_color;
		glm::vec4 ambient_light;
		glm::uvec4 light_counts;
	};

	struct ModelUniforms {
		glm::mat4 model;
		glm::mat4 normal_matrix;
		glm::vec4 albedo;
		glm::vec4 specular;
	};

	namespace {
		// ==== Геометрия ====
		std::vector<Vertex> cylinder_vertices;
		std::vector<uint32_t> cylinder_indices;
		const int SEGMENTS = 50;
		const float RADIUS = 1.0f;
		const float HEIGHT = 2.0f;

		VkBuffer vertex_buffer = VK_NULL_HANDLE;
		VmaAllocation vertex_buffer_allocation = VK_NULL_HANDLE;
		VkBuffer index_buffer = VK_NULL_HANDLE;
		VmaAllocation index_buffer_allocation = VK_NULL_HANDLE;
		uint32_t index_count = 0;

		// ==== Дебаг геометрия ====
		std::vector<DebugVertex> debug_vertices;
		std::vector<uint32_t> debug_indices;
		VkBuffer debug_vertex_buffer = VK_NULL_HANDLE;
		VmaAllocation debug_vertex_alloc = VK_NULL_HANDLE;
		VkBuffer debug_index_buffer = VK_NULL_HANDLE;
		VmaAllocation debug_index_alloc = VK_NULL_HANDLE;
		uint32_t debug_index_count = 0;
		bool show_light_visualizers = true;

		// ==== Пайплайны ====
		VkPipeline graphics_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline debug_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout debug_pipeline_layout = VK_NULL_HANDLE;

		// ==== Дескрипторы ====
		VkDescriptorSetLayout scene_set_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout model_set_layout = VK_NULL_HANDLE;
		VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
		VkDescriptorSet scene_set = VK_NULL_HANDLE;
		VkDescriptorSet model_set = VK_NULL_HANDLE;

		// ==== Uniform/Storage буферы ====
		VkBuffer scene_uniform_buffer = VK_NULL_HANDLE;
		VmaAllocation scene_uniform_alloc = VK_NULL_HANDLE;
		SceneUniforms* scene_uniform_mapped = nullptr;

		VkBuffer model_uniform_buffer = VK_NULL_HANDLE;
		VmaAllocation model_uniform_alloc = VK_NULL_HANDLE;
		ModelUniforms* model_uniform_mapped = nullptr;

		VkBuffer point_light_buffer = VK_NULL_HANDLE;
		VmaAllocation point_light_alloc = VK_NULL_HANDLE;
		PointLightGPU* point_light_mapped = nullptr;

		VkBuffer spot_light_buffer = VK_NULL_HANDLE;
		VmaAllocation spot_light_alloc = VK_NULL_HANDLE;
		SpotLightGPU* spot_light_mapped = nullptr;

		const uint32_t MAX_POINT_LIGHTS = 16;
		const uint32_t MAX_SPOT_LIGHTS = 16;

		// ==== Состояние сцены ====
		glm::vec3 camera_position = glm::vec3(0.0f, 2.0f, 6.0f);
		glm::vec3 camera_target = glm::vec3(0.0f, 0.0f, 0.0f);
		float camera_fov_deg = 45.0f;
		float camera_near = 0.1f;
		float camera_far = 100.0f;
		int projection_mode = 0;
		float ortho_half_height = 2.0f;

		glm::vec3 dir_light_direction = glm::vec3(-0.5f, -1.0f, -0.3f);
		glm::vec3 dir_light_color = glm::vec3(1.0f, 1.0f, 1.0f);
		float dir_light_intensity = 1.0f;
		glm::vec3 ambient_color = glm::vec3(0.15f, 0.15f, 0.2f);

		glm::vec3 material_albedo = glm::vec3(0.8f, 0.3f, 0.3f);
		glm::vec3 material_specular = glm::vec3(1.0f, 1.0f, 1.0f);
		float material_shininess = 32.0f;

		std::vector<PointLightGPU> point_lights;
		std::vector<SpotLightGPU> spot_lights;

		bool anim_playing = true;
		float anim_speed = 1.0f;
		float anim_radius = 1.5f;
		float anim_time = 0.0f;
		double last_frame_time = 0.0;

		// ==== Утилиты ====
		void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, void* data) {
			VkBufferCreateInfo bufferInfo{};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = size;
			bufferInfo.usage = usage;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VmaAllocationCreateInfo allocInfo{};
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

			VmaAllocationInfo allocationInfo;
			vmaCreateBuffer(graphics::internal::context.allocator, &bufferInfo, &allocInfo, &buffer, &allocation, &allocationInfo);
			memcpy(allocationInfo.pMappedData, data, size);
		}

		void createUniformBuffer(VkDeviceSize size, VkBuffer& buffer, VmaAllocation& alloc, void** mapped) {
			VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; info.size = size;
			info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocInfo{}; allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo result;
			vmaCreateBuffer(graphics::internal::context.allocator, &info, &allocInfo, &buffer, &alloc, &result);
			*mapped = result.pMappedData;
		}

		void createStorageBuffer(VkDeviceSize size, VkBuffer& buffer, VmaAllocation& alloc, void** mapped) {
			VkBufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; info.size = size;
			info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocInfo{}; allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo result;
			vmaCreateBuffer(graphics::internal::context.allocator, &info, &allocInfo, &buffer, &alloc, &result);
			*mapped = result.pMappedData;
		}

		// ==== Генерация цилиндра ====
		void generateCylinder() {
			cylinder_vertices.clear();
			cylinder_indices.clear();
			float halfHeight = HEIGHT / 2.0f;
			float angleStep = 2.0f * glm::pi<float>() / SEGMENTS;

			for (int i = 0; i <= SEGMENTS; ++i) {
				float angle = i * angleStep;
				float x = cos(angle) * RADIUS;
				float z = sin(angle) * RADIUS;
				glm::vec3 normal = glm::vec3(cos(angle), 0.0f, sin(angle));
				cylinder_vertices.push_back({ {x, halfHeight, z}, normal, glm::vec3(1.0f) });
				cylinder_vertices.push_back({ {x, -halfHeight, z}, normal, glm::vec3(1.0f) });
			}
			for (int i = 0; i < SEGMENTS; ++i) {
				uint32_t t1 = i * 2, b1 = i * 2 + 1, t2 = (i + 1) * 2, b2 = (i + 1) * 2 + 1;
				cylinder_indices.insert(cylinder_indices.end(), { t1, b1, t2, t2, b1, b2 });
			}

			uint32_t topIdx = cylinder_vertices.size();
			cylinder_vertices.push_back({ {0.0f, halfHeight, 0.0f}, {0.0f, 1.0f, 0.0f}, glm::vec3(1.0f) });
			for (int i = 0; i < SEGMENTS; ++i) {
				cylinder_indices.insert(cylinder_indices.end(), { topIdx, (uint32_t)(i * 2), (uint32_t)((i + 1) * 2) });
			}

			uint32_t botIdx = cylinder_vertices.size();
			cylinder_vertices.push_back({ {0.0f, -halfHeight, 0.0f}, {0.0f, -1.0f, 0.0f}, glm::vec3(1.0f) });
			for (int i = 0; i < SEGMENTS; ++i) {
				cylinder_indices.insert(cylinder_indices.end(), { botIdx, (uint32_t)((i + 1) * 2 + 1), (uint32_t)(i * 2 + 1) });
			}
		}

		// ==== Геометрия для визуализаторов ====
		void appendCube(glm::vec3 center, float size, glm::vec3 color) {
			uint32_t base = debug_vertices.size();
			float h = size * 0.5f;
			glm::vec3 p[8] = {
				center + glm::vec3(-h, -h, -h), center + glm::vec3(h, -h, -h),
				center + glm::vec3(h, h, -h), center + glm::vec3(-h, h, -h),
				center + glm::vec3(-h, -h, h), center + glm::vec3(h, -h, h),
				center + glm::vec3(h, h, h), center + glm::vec3(-h, h, h)
			};
			for (int i = 0; i < 8; ++i) debug_vertices.push_back({ p[i], color });
			uint32_t idx[] = { 0,1,2, 2,3,0, 4,5,6, 6,7,4, 0,4,7, 7,3,0, 1,5,6, 6,2,1, 3,2,6, 6,7,3, 0,1,5, 5,4,0 };
			for (uint32_t i : idx) debug_indices.push_back(base + i);
		}

		void appendCone(glm::vec3 baseCenter, glm::vec3 dir, float height, float radius, glm::vec3 color) {
			uint32_t baseIdx = debug_vertices.size();
			dir = glm::normalize(dir);
			glm::vec3 up = glm::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
			glm::vec3 right = glm::normalize(glm::cross(dir, up));
			up = glm::normalize(glm::cross(right, dir));
			glm::vec3 tip = baseCenter + dir * height;
			debug_vertices.push_back({ tip, color });
			for (int i = 0; i < 4; ++i) {
				float angle = i * glm::pi<float>() / 2.0f;
				glm::vec3 p = baseCenter + right * cos(angle) * radius + up * sin(angle) * radius;
				debug_vertices.push_back({ p, color });
			}
			for (int i = 0; i < 4; ++i) {
				debug_indices.insert(debug_indices.end(), { baseIdx, (uint32_t)(baseIdx + 1 + i), (uint32_t)(baseIdx + 1 + (i + 1) % 4) });
			}
			debug_indices.insert(debug_indices.end(), { baseIdx + 1, baseIdx + 2, baseIdx + 3, baseIdx + 1, baseIdx + 3, baseIdx + 4 });
		}

		void generateDebugGeometry() {
			debug_vertices.clear();
			debug_indices.clear();
			for (const auto& pl : point_lights) {
				if (pl.color.w > 0.01f) appendCube(glm::vec3(pl.position), 0.15f, glm::vec3(pl.color));
			}
			for (const auto& sl : spot_lights) {
				if (sl.color.w > 0.01f) {
					glm::vec3 pos = glm::vec3(sl.position);
					glm::vec3 dir = glm::normalize(glm::vec3(sl.direction));
					appendCone(pos, dir, 2.0f, 2.0f * glm::tan(glm::radians(sl.params.y)), glm::vec3(sl.color));
					appendCube(pos, 0.1f, glm::vec3(sl.color));
				}
			}
		}

		// ==== Файлы шейдеров ====
		std::filesystem::path getExecutableDir() {
#if defined(_WIN32)
			wchar_t buffer[MAX_PATH]; DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
			if (len == 0 || len == MAX_PATH) return std::filesystem::current_path();
			return std::filesystem::path(buffer).parent_path();
#else
			return std::filesystem::current_path();
#endif
		}

		std::filesystem::path findShadersDir() {
			std::filesystem::path dir = getExecutableDir();
			for (int i = 0; i < 6; ++i) {
				std::filesystem::path candidate = dir / "shaders";
				if (std::filesystem::exists(candidate / "shader.vert.spv")) return candidate;
				if (!dir.has_parent_path() || dir == dir.parent_path()) break;
				dir = dir.parent_path();
			}
			return getExecutableDir() / "shaders";
		}

		std::vector<char> readFile(const std::string& filename) {
			std::ifstream file(filename, std::ios::ate | std::ios::binary);
			size_t fileSize = (size_t)file.tellg();
			std::vector<char> buffer(fileSize);
			file.seekg(0); file.read(buffer.data(), fileSize); file.close();
			return buffer;
		}

		VkShaderModule createShaderModule(const std::vector<char>& code) {
			VkShaderModuleCreateInfo createInfo{}; createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = code.size(); createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
			VkShaderModule shaderModule;
			vkCreateShaderModule(graphics::internal::context.device, &createInfo, nullptr, &shaderModule);
			return shaderModule;
		}

		// ==== Создание дескрипторов ====
		void createDescriptors() {
			auto& context = graphics::internal::context;

			VkDescriptorSetLayoutBinding scene_bindings[3] = {};
			scene_bindings[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
			scene_bindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
			scene_bindings[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };

			VkDescriptorSetLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			layout_info.bindingCount = 3; layout_info.pBindings = scene_bindings;
			vkCreateDescriptorSetLayout(context.device, &layout_info, nullptr, &scene_set_layout);

			VkDescriptorSetLayoutBinding model_binding{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
			VkDescriptorSetLayoutCreateInfo model_layout_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			model_layout_info.bindingCount = 1; model_layout_info.pBindings = &model_binding;
			vkCreateDescriptorSetLayout(context.device, &model_layout_info, nullptr, &model_set_layout);

			VkDescriptorPoolSize pool_sizes[] = {
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
				{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
			};
			VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			pool_info.maxSets = 2; pool_info.poolSizeCount = 2; pool_info.pPoolSizes = pool_sizes;
			vkCreateDescriptorPool(context.device, &pool_info, nullptr, &descriptor_pool);

			VkDescriptorSetLayout layouts[2] = { scene_set_layout, model_set_layout };
			VkDescriptorSetAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			alloc_info.descriptorPool = descriptor_pool; alloc_info.descriptorSetCount = 1;
			alloc_info.pSetLayouts = &layouts[0]; vkAllocateDescriptorSets(context.device, &alloc_info, &scene_set);
			alloc_info.pSetLayouts = &layouts[1]; vkAllocateDescriptorSets(context.device, &alloc_info, &model_set);

			createUniformBuffer(sizeof(SceneUniforms), scene_uniform_buffer, scene_uniform_alloc, (void**)&scene_uniform_mapped);
			createUniformBuffer(sizeof(ModelUniforms), model_uniform_buffer, model_uniform_alloc, (void**)&model_uniform_mapped);
			createStorageBuffer(sizeof(PointLightGPU) * MAX_POINT_LIGHTS, point_light_buffer, point_light_alloc, (void**)&point_light_mapped);
			createStorageBuffer(sizeof(SpotLightGPU) * MAX_SPOT_LIGHTS, spot_light_buffer, spot_light_alloc, (void**)&spot_light_mapped);

			VkDescriptorBufferInfo s_ubo{ scene_uniform_buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo p_stor{ point_light_buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo sp_stor{ spot_light_buffer, 0, VK_WHOLE_SIZE };
			VkDescriptorBufferInfo m_ubo{ model_uniform_buffer, 0, VK_WHOLE_SIZE };

			VkWriteDescriptorSet writes[4] = {};
			writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, scene_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &s_ubo, nullptr };
			writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, scene_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &p_stor, nullptr };
			writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, scene_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &sp_stor, nullptr };
			writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, model_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &m_ubo, nullptr };
			vkUpdateDescriptorSets(context.device, 4, writes, 0, nullptr);
		}

		// ==== Создание основного пайплайна ====
		void createPipeline() {
			auto& context = graphics::internal::context;
			auto shadersDir = findShadersDir();
			VkShaderModule vert = createShaderModule(readFile((shadersDir / "shader.vert.spv").string()));
			VkShaderModule frag = createShaderModule(readFile((shadersDir / "shader.frag.spv").string()));

			VkPipelineShaderStageCreateInfo stages[2] = {};
			stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr };
			stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr };

			VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
			std::array<VkVertexInputAttributeDescription, 3> attrs{};
			attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos) };
			attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal) };
			attrs[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color) };

			VkPipelineVertexInputStateCreateInfo vInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vInput.vertexBindingDescriptionCount = 1; vInput.pVertexBindingDescriptions = &binding;
			vInput.vertexAttributeDescriptionCount = 3; vInput.pVertexAttributeDescriptions = attrs.data();

			VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			vp.viewportCount = 1; vp.scissorCount = 1;

			VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rast.polygonMode = VK_POLYGON_MODE_FILL; rast.cullMode = VK_CULL_MODE_BACK_BIT;
			rast.frontFace = VK_FRONT_FACE_CLOCKWISE; rast.lineWidth = 1.0f;

			VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

			VkPipelineColorBlendAttachmentState cba{};
			cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
			VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			cb.attachmentCount = 1; cb.pAttachments = &cba;

			VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

			VkDescriptorSetLayout layouts[2] = { scene_set_layout, model_set_layout };
			VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			plInfo.setLayoutCount = 2; plInfo.pSetLayouts = layouts;
			vkCreatePipelineLayout(context.device, &plInfo, nullptr, &pipeline_layout);

			VkGraphicsPipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			pipeInfo.stageCount = 2; pipeInfo.pStages = stages;
			pipeInfo.pVertexInputState = &vInput; pipeInfo.pInputAssemblyState = &ia;
			pipeInfo.pViewportState = &vp; pipeInfo.pRasterizationState = &rast;
			pipeInfo.pMultisampleState = &ms; pipeInfo.pDepthStencilState = &ds;
			pipeInfo.pColorBlendState = &cb; pipeInfo.pDynamicState = &dyn;
			pipeInfo.layout = pipeline_layout;
			pipeInfo.renderPass = context.render_pass; pipeInfo.subpass = 0;

			vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &graphics_pipeline);
			vkDestroyShaderModule(context.device, vert, nullptr);
			vkDestroyShaderModule(context.device, frag, nullptr);
		}

		// ==== Создание пайплайна визуализации ====
		void createDebugPipeline() {
			auto& context = graphics::internal::context;
			auto shadersDir = findShadersDir();
			VkShaderModule vert = createShaderModule(readFile((shadersDir / "debug.vert.spv").string()));
			VkShaderModule frag = createShaderModule(readFile((shadersDir / "debug.frag.spv").string()));

			VkPipelineShaderStageCreateInfo stages[2] = {};
			stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr };
			stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr };

			VkVertexInputBindingDescription binding{ 0, sizeof(DebugVertex), VK_VERTEX_INPUT_RATE_VERTEX };
			VkVertexInputAttributeDescription attrs[2] = {
				{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugVertex, pos) },
				{ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(DebugVertex, color) }
			};

			VkPipelineVertexInputStateCreateInfo vInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			vInput.vertexBindingDescriptionCount = 1; vInput.pVertexBindingDescriptions = &binding;
			vInput.vertexAttributeDescriptionCount = 2; vInput.pVertexAttributeDescriptions = attrs;

			VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			vp.viewportCount = 1; vp.scissorCount = 1;

			VkPipelineRasterizationStateCreateInfo rast{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			rast.polygonMode = VK_POLYGON_MODE_FILL; rast.cullMode = VK_CULL_MODE_NONE; // Важно: без отсечения
			rast.frontFace = VK_FRONT_FACE_CLOCKWISE; rast.lineWidth = 1.0f;

			VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;

			VkPipelineColorBlendAttachmentState cba{};
			cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
			VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			cb.attachmentCount = 1; cb.pAttachments = &cba;

			VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynStates;

			VkPipelineLayoutCreateInfo plInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			plInfo.setLayoutCount = 1; plInfo.pSetLayouts = &scene_set_layout; // Используем только set 0
			vkCreatePipelineLayout(context.device, &plInfo, nullptr, &debug_pipeline_layout);

			VkGraphicsPipelineCreateInfo pipeInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			pipeInfo.stageCount = 2; pipeInfo.pStages = stages;
			pipeInfo.pVertexInputState = &vInput; pipeInfo.pInputAssemblyState = &ia;
			pipeInfo.pViewportState = &vp; pipeInfo.pRasterizationState = &rast;
			pipeInfo.pMultisampleState = &ms; pipeInfo.pDepthStencilState = &ds;
			pipeInfo.pColorBlendState = &cb; pipeInfo.pDynamicState = &dyn;
			pipeInfo.layout = debug_pipeline_layout;
			pipeInfo.renderPass = context.render_pass; pipeInfo.subpass = 0;

			vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &debug_pipeline);
			vkDestroyShaderModule(context.device, vert, nullptr);
			vkDestroyShaderModule(context.device, frag, nullptr);
		}
	}

	// ==== Публичные функции ====

	bool initialize() {
		generateCylinder();
		VkDeviceSize vSize = sizeof(Vertex) * cylinder_vertices.size();
		createBuffer(vSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_buffer_allocation, cylinder_vertices.data());
		VkDeviceSize iSize = sizeof(uint32_t) * cylinder_indices.size();
		createBuffer(iSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_buffer_allocation, cylinder_indices.data());
		index_count = static_cast<uint32_t>(cylinder_indices.size());

		createDescriptors();
		createPipeline();
		createDebugPipeline();
		return true;
	}

	void shutdown() {
		auto& context = graphics::internal::context;
		vkQueueWaitIdle(context.graphics_queue);

		vmaDestroyBuffer(context.allocator, vertex_buffer, vertex_buffer_allocation);
		vmaDestroyBuffer(context.allocator, index_buffer, index_buffer_allocation);
		if (debug_vertex_buffer != VK_NULL_HANDLE) vmaDestroyBuffer(context.allocator, debug_vertex_buffer, debug_vertex_alloc);
		if (debug_index_buffer != VK_NULL_HANDLE) vmaDestroyBuffer(context.allocator, debug_index_buffer, debug_index_alloc);
		vmaDestroyBuffer(context.allocator, scene_uniform_buffer, scene_uniform_alloc);
		vmaDestroyBuffer(context.allocator, model_uniform_buffer, model_uniform_alloc);
		vmaDestroyBuffer(context.allocator, point_light_buffer, point_light_alloc);
		vmaDestroyBuffer(context.allocator, spot_light_buffer, spot_light_alloc);

		vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
		vkDestroyDescriptorSetLayout(context.device, scene_set_layout, nullptr);
		vkDestroyDescriptorSetLayout(context.device, model_set_layout, nullptr);

		vkDestroyPipeline(context.device, graphics_pipeline, nullptr);
		vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
		vkDestroyPipeline(context.device, debug_pipeline, nullptr);
		vkDestroyPipelineLayout(context.device, debug_pipeline_layout, nullptr);
	}

	void update(double time) {
		double dt = (last_frame_time > 0.0) ? (time - last_frame_time) : 0.0;
		last_frame_time = time;
		if (anim_playing) anim_time += static_cast<float>(dt) * anim_speed;

		ImGui::Begin("Lab 2: Lighting & Camera");

		ImGui::SeparatorText("Projection");
		ImGui::RadioButton("Perspective", &projection_mode, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Orthographic", &projection_mode, 1);
		if (projection_mode == 0) ImGui::SliderFloat("FOV (deg)", &camera_fov_deg, 10.0f, 120.0f);
		else ImGui::SliderFloat("Ortho half-height", &ortho_half_height, 0.5f, 10.0f);

		ImGui::SeparatorText("Camera");
		ImGui::DragFloat3("Position", &camera_position.x, 0.05f);
		ImGui::DragFloat3("Target", &camera_target.x, 0.05f);

		ImGui::SeparatorText("Material");
		ImGui::ColorEdit3("Albedo", &material_albedo.x);
		ImGui::ColorEdit3("Specular", &material_specular.x);
		ImGui::SliderFloat("Shininess", &material_shininess, 1.0f, 256.0f);

		ImGui::SeparatorText("Ambient");
		ImGui::ColorEdit3("Ambient", &ambient_color.x);

		ImGui::SeparatorText("Directional Light");
		ImGui::DragFloat3("Direction", &dir_light_direction.x, 0.02f);
		ImGui::ColorEdit3("Color##dir", &dir_light_color.x);
		ImGui::SliderFloat("Intensity##dir", &dir_light_intensity, 0.0f, 5.0f);

		ImGui::SeparatorText("Point Lights");
		if (ImGui::Button("Add Point Light") && point_lights.size() < MAX_POINT_LIGHTS) {
			PointLightGPU p{};
			p.position = glm::vec4(2.0f, 1.0f, 2.0f, 0.0f);
			p.color = glm::vec4(1.0f, 0.5f, 0.2f, 2.0f);
			point_lights.push_back(p);
		}
		for (size_t i = 0; i < point_lights.size(); ++i) {
			ImGui::PushID((int)i);
			ImGui::Text("Point Light #%d", (int)i);
			ImGui::DragFloat3("Pos", &point_lights[i].position.x, 0.05f);
			ImGui::ColorEdit3("Color", &point_lights[i].color.x);
			ImGui::SliderFloat("Intensity", &point_lights[i].color.w, 0.0f, 10.0f);
			if (ImGui::Button("Remove")) { point_lights.erase(point_lights.begin() + i); ImGui::PopID(); break; }
			ImGui::PopID();
		}

		ImGui::SeparatorText("Spot Lights");
		if (ImGui::Button("Add Spot Light") && spot_lights.size() < MAX_SPOT_LIGHTS) {
			SpotLightGPU s{};
			s.position = glm::vec4(0.0f, 5.0f, 0.0f, 0.0f);
			s.color = glm::vec4(0.3f, 0.6f, 1.0f, 3.0f);
			s.direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
			s.params = glm::vec4(15.0f, 25.0f, 0.0f, 0.0f);
			spot_lights.push_back(s);
		}
		for (size_t i = 0; i < spot_lights.size(); ++i) {
			ImGui::PushID((int)i + 1000);
			ImGui::Text("Spot Light #%d", (int)i);
			ImGui::DragFloat3("Pos", &spot_lights[i].position.x, 0.05f);
			ImGui::DragFloat3("Dir", &spot_lights[i].direction.x, 0.02f);
			ImGui::ColorEdit3("Color", &spot_lights[i].color.x);
			ImGui::SliderFloat("Intensity", &spot_lights[i].color.w, 0.0f, 10.0f);
			ImGui::SliderFloat("Inner", &spot_lights[i].params.x, 0.0f, 45.0f);
			ImGui::SliderFloat("Outer", &spot_lights[i].params.y, 0.0f, 60.0f);
			if (ImGui::Button("Remove")) { spot_lights.erase(spot_lights.begin() + i); ImGui::PopID(); break; }
			ImGui::PopID();
		}

		ImGui::SeparatorText("Animation");
		if (ImGui::Button(anim_playing ? "Pause" : "Play")) anim_playing = !anim_playing;
		ImGui::SliderFloat("Speed", &anim_speed, 0.0f, 5.0f);
		ImGui::SliderFloat("Radius", &anim_radius, 0.0f, 4.0f);

		ImGui::SeparatorText("Visualizers");
		ImGui::Checkbox("Show Light Visualizers", &show_light_visualizers);

		ImGui::End();
	}

	void render(const graphics::internal::FrameData& fd) {
		auto& context = graphics::internal::context;
		VkCommandBuffer cmd = fd.command_buffer;

		// 1. Матрицы
		glm::mat4 view = glm::lookAt(camera_position, camera_target, glm::vec3(0.0f, 1.0f, 0.0f));
		float aspect = (float)context.swapchain_extent.width / (float)context.swapchain_extent.height;
		glm::mat4 proj;
		if (projection_mode == 0) proj = glm::perspective(glm::radians(camera_fov_deg), aspect, camera_near, camera_far);
		else { float h = ortho_half_height; proj = glm::ortho(-h * aspect, h * aspect, -h, h, camera_near, camera_far); }
		proj[1][1] *= -1;

		scene_uniform_mapped->view_projection = proj * view;
		scene_uniform_mapped->camera_position = glm::vec4(camera_position, 0.0f);
		scene_uniform_mapped->directional_direction = glm::vec4(dir_light_direction, 0.0f);
		scene_uniform_mapped->directional_color = glm::vec4(dir_light_color, dir_light_intensity);
		scene_uniform_mapped->ambient_light = glm::vec4(ambient_color, 0.0f);
		scene_uniform_mapped->light_counts = glm::uvec4((uint32_t)point_lights.size(), (uint32_t)spot_lights.size(), 0, 0);

		glm::vec3 orbit_offset(0.0f);
		if (anim_radius > 0.0f) {
			orbit_offset = glm::vec3(anim_radius * cos(anim_time), anim_radius * 0.5f * sin(2.0f * anim_time), anim_radius * sin(anim_time));
		}
		glm::mat4 model = glm::translate(glm::mat4(1.0f), orbit_offset);
		model_uniform_mapped->model = model;
		model_uniform_mapped->normal_matrix = glm::transpose(glm::inverse(model));
		model_uniform_mapped->albedo = glm::vec4(material_albedo, 1.0f);
		model_uniform_mapped->specular = glm::vec4(material_specular, material_shininess);

		if (!point_lights.empty()) memcpy(point_light_mapped, point_lights.data(), point_lights.size() * sizeof(PointLightGPU));
		if (!spot_lights.empty()) memcpy(spot_light_mapped, spot_lights.data(), spot_lights.size() * sizeof(SpotLightGPU));

		// 2. Render Pass
		VkRenderPassBeginInfo renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		renderPassInfo.renderPass = context.render_pass;
		renderPassInfo.framebuffer = fd.framebuffer;
		renderPassInfo.renderArea = { {0, 0}, context.swapchain_extent };
		std::array<VkClearValue, 2> clearValues{};
		clearValues[0].color = { {0.05f, 0.05f, 0.1f, 1.0f} }; // Тёмно-синий фон
		clearValues[1].depthStencil = { 1.0f, 0 };
		renderPassInfo.clearValueCount = 2; renderPassInfo.pClearValues = clearValues.data();
		vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport{ 0, 0, (float)context.swapchain_extent.width, (float)context.swapchain_extent.height, 0.0f, 1.0f };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		VkRect2D scissor{ {0, 0}, context.swapchain_extent };
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		// 3. Рисуем цилиндр
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
		VkDescriptorSet sets[2] = { scene_set, model_set };
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2, sets, 0, nullptr);
		VkBuffer vBuffers[] = { vertex_buffer };
		VkDeviceSize offsets[] = { 0 };
		vkCmdBindVertexBuffers(cmd, 0, 1, vBuffers, offsets);
		vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);

		// 4. Рисуем визуализаторы
		if (show_light_visualizers) {
			generateDebugGeometry();

			if (debug_vertex_buffer != VK_NULL_HANDLE) {
				vmaDestroyBuffer(context.allocator, debug_vertex_buffer, debug_vertex_alloc);
				debug_vertex_buffer = VK_NULL_HANDLE;
			}
			if (debug_index_buffer != VK_NULL_HANDLE) {
				vmaDestroyBuffer(context.allocator, debug_index_buffer, debug_index_alloc);
				debug_index_buffer = VK_NULL_HANDLE;
			}
			debug_index_count = 0;

			if (!debug_vertices.empty()) {
				VkDeviceSize sz = sizeof(DebugVertex) * debug_vertices.size();
				createBuffer(sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, debug_vertex_buffer, debug_vertex_alloc, debug_vertices.data());
			}
			if (!debug_indices.empty()) {
				VkDeviceSize sz = sizeof(uint32_t) * debug_indices.size();
				createBuffer(sz, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, debug_index_buffer, debug_index_alloc, debug_indices.data());
				debug_index_count = debug_indices.size();
			}

			if (debug_index_count > 0) {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_pipeline);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_pipeline_layout, 0, 1, &scene_set, 0, nullptr);
				VkBuffer dbgVB[] = { debug_vertex_buffer };
				vkCmdBindVertexBuffers(cmd, 0, 1, dbgVB, offsets);
				vkCmdBindIndexBuffer(cmd, debug_index_buffer, 0, VK_INDEX_TYPE_UINT32);
				vkCmdDrawIndexed(cmd, debug_index_count, 1, 0, 0, 0);
			}
		}

		vkCmdEndRenderPass(cmd);
	}
}