These are small, project-authored Metal test programs, covered by the repository's
MIT license. They contain no translated game shaders or captured game data.

| Fixture | Coverage |
| --- | --- |
| `vertex_id_quad` | Four corners selected by vertex ID. |
| `uniform_color` | A color read through an argument-buffer uniform pointer. |
| `position_uv` | Position and UV attributes from vertex streams. |
| `sample_texture` | A texture and paired sampler in an argument buffer. |
| `copy_texture` | A bounded compute copy using uniforms, a support constant, a sampled texture, and a writable image. |
| `vertex_color_transform` | A 2D transform and vertex color multiplied by a separate mask attribute. |
| `adjust_vertex_color` | Uniform color scale and bias. |

The JSON files describe the bindings expected by the renderer. They are written
alongside the fixtures; their names are not game shader cache keys.

`t_nvn_commands`, `t_nvn_render_state`, and `bench_nvn_pipeline` use these programs
to check actual GPU pixels, depth/stencil state, vertex/index inputs, argument
buffers, and command lifetime. The compute check compares every copied texel.
