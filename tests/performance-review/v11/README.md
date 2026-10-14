# Validated rendering performance changes (V11-AB)

Ordinary graphics draws record a smaller command without texture-table ownership and push the final rescaling/render-area bytes together. Texture-table draws retain their ownership and descriptor behavior. Register updates combine two low dirty bits using the public bitset API, with the original high-bit behavior. Resource allocation first checks whether the cached completion tick already proves the hinted slot reusable; otherwise it follows the original refresh/search/grow path.

Run `run.ps1 -Python <python> -VulkanSdk <SDK>` in a Visual Studio developer PowerShell. The script extracts the current production command and helpers. The reference implementations are preserved in the fixtures. It checks 48,000 deferred draw cases (raw constant bits, descriptor traces, command rollover and ownership), 40,000 register/shadow cases, and 2.4 million pool selection/tick/wrap/growth cases. Small runtime and driver adapters make these host tests, not a full emulator replay.

Additional GPU validation before packaging: 28,672 draws reading all seven constant words and 256 descriptor dispatches across 32 asynchronous submissions produced the expected pixels/values with zero Vulkan validation errors. In-game V11-AB was validated by the user in Mario Odyssey and Immortals, including Immortals textures, shadows and geometry.

One matched Mario F9 comparison, excluding the first five seconds (5–28 s): A 197.32 FPS / 153.99 FPS 1% low; B 197.04 / 150.83; AB 199.01 / 157.78. These are capture averages, not the displayed instantaneous FPS. A single comparison does not establish a universal gain; a later dip also occurred in the longer AB run.

The commit excludes the unvalidated V12 empty-vertex optimization and all Cappy CPU experiments. It changes no guest code or IPS patch.
