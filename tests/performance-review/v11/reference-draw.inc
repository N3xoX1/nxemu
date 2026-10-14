    scheduler.Record([this, descriptor_data, bind_pipeline, rescaling_data = rescaling.Data(),
                      is_rescaling, update_rescaling,
                      table_data = uses_texture_table ? texture_table_data : nullptr,
                      table_version = texture_table_version,
                      uses_render_area = render_area.uses_render_area,
                      render_area_data = render_area.words](vk::CommandBuffer cmdbuf) {
        if (!IsBuilt()) {
            return;
        }
        if (bind_pipeline) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
        }
        cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                             RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                             rescaling_data.data());
        if (update_rescaling || bind_pipeline) {
            const f32 config_down_factor{videoSettings.resolution_info.down_factor};
            const f32 scale_down_factor{is_rescaling ? config_down_factor : 1.0f};
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                                 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, sizeof(scale_down_factor),
                                 &scale_down_factor);
        }
        if (uses_render_area) {
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                                 RENDERAREA_LAYOUT_OFFSET, sizeof(render_area_data),
                                 &render_area_data);
        }
        if (table_data) {
            if (texture_table_set_version != table_version) {
                texture_table_set = texture_table_allocator.Commit();
                boost::container::small_vector<VkWriteDescriptorSet, 4> writes;
                const VkDescriptorImageInfo* entries{table_data->data()};
                for (const auto& binding : texture_table_bindings) {
                    writes.push_back({
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .pNext = nullptr,
                        .dstSet = texture_table_set,
                        .dstBinding = binding.binding,
                        .dstArrayElement = 0,
                        .descriptorCount = binding.descriptorCount,
                        .descriptorType = binding.descriptorType,
                        .pImageInfo = entries,
                        .pBufferInfo = nullptr,
                        .pTexelBufferView = nullptr,
                    });
                    entries += binding.descriptorCount;
                }
                device.GetLogical().UpdateDescriptorSets(writes, {});
                texture_table_set_version = table_version;
            }
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout, 1,
                                      texture_table_set, nullptr);
        }
        // An empty set 0 has a layout but nothing to update
        if (!descriptor_update_template) {
            return;
        }
        if (uses_push_descriptor) {
            cmdbuf.PushDescriptorSetWithTemplateKHR(*descriptor_update_template, *pipeline_layout,
                                                    0, descriptor_data);
        } else {
            const VkDescriptorSet descriptor_set{descriptor_allocator.Commit()};
            const vk::Device& dev{device.GetLogical()};
            dev.UpdateDescriptorSet(descriptor_set, *descriptor_update_template, descriptor_data);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout, 0,
                                      descriptor_set, nullptr);
        }
    });
