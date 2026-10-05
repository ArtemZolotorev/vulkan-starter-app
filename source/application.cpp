// это обязательно нужно до инклюда glm, иначе glm думает что мы на OpenGL
// и ortho проекция ломается (у меня вообще цилиндр пропадал)
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

	// одна вершина цилиндра
	struct Vertex {
		glm::vec3 pos;   // позиция x y z
		glm::vec3 color; // цвет r g b
	};

	// то что кидаем в шейдер через push constants (без uniform buffer, т.к. данных мало)
	// порядок полей должен совпадать с тем что в shader.vert!
	struct PushConstants {
		glm::mat4 mvp;
		glm::vec4 tint; // цвет который выбрали в интерфейсе
	};

	namespace {
		std::vector<Vertex> cylinder_vertices;
		std::vector<uint32_t> cylinder_indices;

		// параметры цилиндра
		const int SEGMENTS = 50;
		const float RADIUS = 1.0f;
		const float HEIGHT = 2.0f;

		VkBuffer vertex_buffer = VK_NULL_HANDLE;
		VmaAllocation vertex_buffer_allocation = VK_NULL_HANDLE;

		VkBuffer index_buffer = VK_NULL_HANDLE;
		VmaAllocation index_buffer_allocation = VK_NULL_HANDLE;

		uint32_t index_count = 0;

		VkPipeline graphics_pipeline = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

		// ===== всё что крутится в интерфейсе =====

		// переключалка проекции (0 - перспектива, 1 - орто)
		int projection_mode = 0;
		float fov_degrees = 45.0f;
		float ortho_half_height = 2.0f;

		// позиция/поворот/масштаб фигуры, крутим ползунками
		glm::vec3 object_position = glm::vec3(0.0f);
		glm::vec3 object_rotation_deg = glm::vec3(0.0f);
		glm::vec3 object_scale = glm::vec3(1.0f);

		// анимация - фигура летает по кривой (не просто по кругу, а как восьмерка)
		bool anim_playing = true;
		float anim_speed = 1.0f;
		float anim_radius = 1.5f;
		float anim_time = 0.0f; // сколько уже "проиграно" анимации
		double last_frame_time = 0.0;

		// цвет который выбираем в ColorEdit, умножается на цвет вершин
		glm::vec3 tint_color = glm::vec3(1.0f, 1.0f, 1.0f);

		// ==========================================

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

		// цвет вершины считаем из ее локальных координат - получается градиент по цилиндру
		glm::vec3 proceduralVertexColor(float x, float y, float z, float halfHeight) {
			return glm::vec3(
				x / RADIUS * 0.5f + 0.5f,
				y / halfHeight * 0.5f + 0.5f,
				z / RADIUS * 0.5f + 0.5f
			);
		}

		void generateCylinder() {
			cylinder_vertices.clear();
			cylinder_indices.clear();

			float halfHeight = HEIGHT / 2.0f;
			float angleStep = 2.0f * glm::pi<float>() / SEGMENTS;

			// боковая поверхность
			for (int i = 0; i <= SEGMENTS; ++i) {
				float angle = i * angleStep;
				float x = cos(angle) * RADIUS;
				float z = sin(angle) * RADIUS;

				cylinder_vertices.push_back({ {x, halfHeight, z}, proceduralVertexColor(x, halfHeight, z, halfHeight) });
				cylinder_vertices.push_back({ {x, -halfHeight, z}, proceduralVertexColor(x, -halfHeight, z, halfHeight) });
			}

			// индексы боковой поверхности, по 2 треугольника на сегмент
			for (int i = 0; i < SEGMENTS; ++i) {
				uint32_t top1 = i * 2;
				uint32_t bottom1 = i * 2 + 1;
				uint32_t top2 = (i + 1) * 2;
				uint32_t bottom2 = (i + 1) * 2 + 1;

				cylinder_indices.push_back(top1);
				cylinder_indices.push_back(bottom1);
				cylinder_indices.push_back(top2);

				cylinder_indices.push_back(top2);
				cylinder_indices.push_back(bottom1);
				cylinder_indices.push_back(bottom2);
			}

			// верхняя крышка (веером от центра)
			uint32_t topCenterIndex = static_cast<uint32_t>(cylinder_vertices.size());
			cylinder_vertices.push_back({ {0.0f, halfHeight, 0.0f}, proceduralVertexColor(0.0f, halfHeight, 0.0f, halfHeight) });

			for (int i = 0; i < SEGMENTS; ++i) {
				uint32_t currentTop = i * 2;
				uint32_t nextTop = (i + 1) * 2;

				cylinder_indices.push_back(topCenterIndex);
				cylinder_indices.push_back(currentTop);
				cylinder_indices.push_back(nextTop);
			}

			// нижняя крышка, тут обход вершин в другую сторону чтобы не отсекалось culling'ом
			uint32_t bottomCenterIndex = static_cast<uint32_t>(cylinder_vertices.size());
			cylinder_vertices.push_back({ {0.0f, -halfHeight, 0.0f}, proceduralVertexColor(0.0f, -halfHeight, 0.0f, halfHeight) });

			for (int i = 0; i < SEGMENTS; ++i) {
				uint32_t currentBottom = i * 2 + 1;
				uint32_t nextBottom = (i + 1) * 2 + 1;

				cylinder_indices.push_back(bottomCenterIndex);
				cylinder_indices.push_back(nextBottom);
				cylinder_indices.push_back(currentBottom);
			}
		}

		// ищем папку shaders рядом с exe (чтобы не зависеть от того откуда запущено)
		std::filesystem::path getExecutableDir() {
#if defined(_WIN32)
			wchar_t buffer[MAX_PATH];
			DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
			if (len == 0 || len == MAX_PATH) {
				return std::filesystem::current_path();
			}
			return std::filesystem::path(buffer).parent_path();
#else
			return std::filesystem::current_path();
#endif
		}

		// поднимаемся вверх по папкам пока не найдем shaders с нужными файлами
		std::filesystem::path findShadersDir() {
			std::filesystem::path dir = getExecutableDir();
			for (int i = 0; i < 6; ++i) {
				std::filesystem::path candidate = dir / "shaders";
				if (std::filesystem::exists(candidate / "shader.vert.spv") &&
					std::filesystem::exists(candidate / "shader.frag.spv")) {
					return candidate;
				}
				if (!dir.has_parent_path() || dir == dir.parent_path()) break;
				dir = dir.parent_path();
			}
			return getExecutableDir() / "shaders";
		}

		std::vector<char> readFile(const std::string& filename) {
			std::ifstream file(filename, std::ios::ate | std::ios::binary);
			size_t fileSize = (size_t)file.tellg();
			std::vector<char> buffer(fileSize);
			file.seekg(0);
			file.read(buffer.data(), fileSize);
			file.close();
			return buffer;
		}

		VkShaderModule createShaderModule(const std::vector<char>& code) {
			VkShaderModuleCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			createInfo.codeSize = code.size();
			createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
			VkShaderModule shaderModule;
			vkCreateShaderModule(graphics::internal::context.device, &createInfo, nullptr, &shaderModule);
			return shaderModule;
		}

		void createPipeline() {
			auto& context = graphics::internal::context;

			// шейдеры читаем не по относительному пути, а ищем рядом с exe
			std::filesystem::path shadersDir = findShadersDir();
			auto vertShaderCode = readFile((shadersDir / "shader.vert.spv").string());
			auto fragShaderCode = readFile((shadersDir / "shader.frag.spv").string());

			VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
			VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

			VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
			vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
			vertShaderStageInfo.module = vertShaderModule;
			vertShaderStageInfo.pName = "main";

			VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
			fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			fragShaderStageInfo.module = fragShaderModule;
			fragShaderStageInfo.pName = "main";

			VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

			// как парсить нашу структуру Vertex
			VkVertexInputBindingDescription bindingDescription{};
			bindingDescription.binding = 0;
			bindingDescription.stride = sizeof(Vertex);
			bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

			std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
			attributeDescriptions[0].binding = 0;
			attributeDescriptions[0].location = 0;
			attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
			attributeDescriptions[0].offset = offsetof(Vertex, pos);

			attributeDescriptions[1].binding = 0;
			attributeDescriptions[1].location = 1;
			attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
			attributeDescriptions[1].offset = offsetof(Vertex, color);

			VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
			vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
			vertexInputInfo.vertexBindingDescriptionCount = 1;
			vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
			vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
			vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

			// рисуем обычными треугольниками
			VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
			inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
			inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			inputAssembly.primitiveRestartEnable = VK_FALSE;

			// viewport/scissor задаются динамически, тут просто заглушка
			VkPipelineViewportStateCreateInfo viewportState{};
			viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
			viewportState.viewportCount = 1;
			viewportState.scissorCount = 1;

			VkPipelineRasterizationStateCreateInfo rasterizer{};
			rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
			rasterizer.depthClampEnable = VK_FALSE;
			rasterizer.rasterizerDiscardEnable = VK_FALSE;
			rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
			rasterizer.lineWidth = 1.0f;
			rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
			// ниже у нас proj[1][1] *= -1, из-за этого вершины на экране идут в другую
			// сторону, поэтому тут CLOCKWISE а не CCW (без этого цилиндр не рисовался)
			rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
			rasterizer.depthBiasEnable = VK_FALSE;

			VkPipelineMultisampleStateCreateInfo multisampling{};
			multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
			multisampling.sampleShadingEnable = VK_FALSE;
			multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

			VkPipelineDepthStencilStateCreateInfo depthStencil{};
			depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
			depthStencil.depthTestEnable = VK_TRUE;
			depthStencil.depthWriteEnable = VK_TRUE;
			depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
			depthStencil.depthBoundsTestEnable = VK_FALSE;
			depthStencil.stencilTestEnable = VK_FALSE;

			VkPipelineColorBlendAttachmentState colorBlendAttachment{};
			colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			colorBlendAttachment.blendEnable = VK_FALSE;

			VkPipelineColorBlendStateCreateInfo colorBlending{};
			colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
			colorBlending.logicOpEnable = VK_FALSE;
			colorBlending.attachmentCount = 1;
			colorBlending.pAttachments = &colorBlendAttachment;

			std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo dynamicState{};
			dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
			dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
			dynamicState.pDynamicStates = dynamicStates.data();

			// пушим в шейдер mvp матрицу + цвет, поэтому размер структуры а не mat4
			VkPushConstantRange pushConstantRange{};
			pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
			pushConstantRange.offset = 0;
			pushConstantRange.size = sizeof(PushConstants);

			VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
			pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutInfo.setLayoutCount = 0;
			pipelineLayoutInfo.pushConstantRangeCount = 1;
			pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

			vkCreatePipelineLayout(context.device, &pipelineLayoutInfo, nullptr, &pipeline_layout);

			VkGraphicsPipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
			pipelineInfo.stageCount = 2;
			pipelineInfo.pStages = shaderStages;
			pipelineInfo.pVertexInputState = &vertexInputInfo;
			pipelineInfo.pInputAssemblyState = &inputAssembly;
			pipelineInfo.pViewportState = &viewportState;
			pipelineInfo.pRasterizationState = &rasterizer;
			pipelineInfo.pMultisampleState = &multisampling;
			pipelineInfo.pDepthStencilState = &depthStencil;
			pipelineInfo.pColorBlendState = &colorBlending;
			pipelineInfo.pDynamicState = &dynamicState;
			pipelineInfo.layout = pipeline_layout;
			pipelineInfo.renderPass = context.render_pass;
			pipelineInfo.subpass = 0;

			vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphics_pipeline);

			// шейдерные модули после создания пайплайна уже не нужны
			vkDestroyShaderModule(context.device, fragShaderModule, nullptr);
			vkDestroyShaderModule(context.device, vertShaderModule, nullptr);
		}
	}

	bool initialize() {
		generateCylinder();

		VkDeviceSize vertexBufferSize = sizeof(Vertex) * cylinder_vertices.size();
		createBuffer(vertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertex_buffer, vertex_buffer_allocation, cylinder_vertices.data());

		VkDeviceSize indexBufferSize = sizeof(uint32_t) * cylinder_indices.size();
		createBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer, index_buffer_allocation, cylinder_indices.data());

		index_count = static_cast<uint32_t>(cylinder_indices.size());

		createPipeline();

		return true;
	}

	void shutdown() {
		auto& context = graphics::internal::context;
		vkQueueWaitIdle(context.graphics_queue);

		vmaDestroyBuffer(context.allocator, vertex_buffer, vertex_buffer_allocation);
		vmaDestroyBuffer(context.allocator, index_buffer, index_buffer_allocation);

		vkDestroyPipeline(context.device, graphics_pipeline, nullptr);
		vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
	}

	void update(double time) {
		// дельта времени, чтобы анимация не зависела от fps
		double dt = (last_frame_time > 0.0) ? (time - last_frame_time) : 0.0;
		last_frame_time = time;

		if (anim_playing) {
			anim_time += static_cast<float>(dt) * anim_speed;
		}

		ImGui::Begin("Cylinder controls");

		// проекция
		ImGui::SeparatorText("Projection");
		ImGui::RadioButton("Perspective", &projection_mode, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Orthographic", &projection_mode, 1);
		if (projection_mode == 0) {
			ImGui::SliderFloat("FOV (deg)", &fov_degrees, 10.0f, 120.0f);
		}
		else {
			ImGui::SliderFloat("Ortho half-height", &ortho_half_height, 0.5f, 10.0f);
		}

		// позиция / поворот / масштаб
		ImGui::SeparatorText("Transform");
		ImGui::DragFloat3("Position", &object_position.x, 0.02f);
		ImGui::DragFloat3("Rotation (deg)", &object_rotation_deg.x, 0.5f);
		ImGui::DragFloat3("Scale", &object_scale.x, 0.01f, 0.05f, 5.0f);

		// анимация
		ImGui::SeparatorText("Animation (Lissajous path)");
		if (ImGui::Button(anim_playing ? "Pause" : "Play")) {
			anim_playing = !anim_playing;
		}
		ImGui::SameLine();
		ImGui::Text(anim_playing ? "playing" : "paused");
		ImGui::SliderFloat("Speed", &anim_speed, 0.0f, 5.0f);
		ImGui::SliderFloat("Radius", &anim_radius, 0.0f, 4.0f);

		// цвет
		ImGui::SeparatorText("Color");
		ImGui::ColorEdit3("Tint (multiplies vertex color)", &tint_color.x);

		ImGui::End();
	}

	void render(const graphics::internal::FrameData& fd) {
		auto& context = graphics::internal::context;
		VkCommandBuffer cmd = fd.command_buffer;

		VkRenderPassBeginInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassInfo.renderPass = context.render_pass;
		renderPassInfo.framebuffer = fd.framebuffer;
		renderPassInfo.renderArea.offset = { 0, 0 };
		renderPassInfo.renderArea.extent = context.swapchain_extent;

		std::array<VkClearValue, 2> clearValues{};
		clearValues[0].color = { {0.0f, 0.0f, 0.0f, 1.0f} };
		clearValues[1].depthStencil = { 1.0f, 0 };
		renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
		renderPassInfo.pClearValues = clearValues.data();

		vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = (float)context.swapchain_extent.width;
		viewport.height = (float)context.swapchain_extent.height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = context.swapchain_extent;
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		VkBuffer vertexBuffers[] = { vertex_buffer };
		VkDeviceSize offsets[] = { 0 };
		vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
		vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);

		// считаем куда сдвинута фигура по траектории 
		glm::vec3 orbit_offset(0.0f);
		if (anim_radius > 0.0f) {
			orbit_offset = glm::vec3(
				anim_radius * cos(anim_time),
				anim_radius * 0.5f * sin(2.0f * anim_time),
				anim_radius * sin(anim_time)
			);
		}
		float orbit_spin_deg = glm::degrees(anim_time); // плюс она еще сама крутится

		// собираем модельную матрицу: сначала двигаем, потом крутим, потом масштабируем
		glm::mat4 model = glm::translate(glm::mat4(1.0f), object_position + orbit_offset);
		model = glm::rotate(model, glm::radians(orbit_spin_deg), glm::vec3(0.0f, 1.0f, 0.0f));
		model = glm::rotate(model, glm::radians(object_rotation_deg.x), glm::vec3(1.0f, 0.0f, 0.0f));
		model = glm::rotate(model, glm::radians(object_rotation_deg.y), glm::vec3(0.0f, 1.0f, 0.0f));
		model = glm::rotate(model, glm::radians(object_rotation_deg.z), glm::vec3(0.0f, 0.0f, 1.0f));
		model = glm::scale(model, object_scale);

		glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 2.0f, 6.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

		float aspect = (float)context.swapchain_extent.width / (float)context.swapchain_extent.height;

		// тут выбираем какую проекцию считать, в зависимости от переключателя в UI
		glm::mat4 proj;
		if (projection_mode == 0) {
			proj = glm::perspective(glm::radians(fov_degrees), aspect, 0.1f, 100.0f);
		}
		else {
			float h = ortho_half_height;
			proj = glm::ortho(-h * aspect, h * aspect, -h, h, 0.1f, 100.0f);
		}
		proj[1][1] *= -1; // вулкан и opengl по-разному считают ось Y, вот этот фикс

		PushConstants pc{};
		pc.mvp = proj * view * model;
		pc.tint = glm::vec4(tint_color, 1.0f);

		vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &pc);

		vkCmdDrawIndexed(cmd, index_count, 1, 0, 0, 0);

		vkCmdEndRenderPass(cmd);
	}

} // namespace application