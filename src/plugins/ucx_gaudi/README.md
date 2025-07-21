# UCX Gaudi Backend Plugin

The UCX Gaudi backend plugin provides optimized UCX-based communication specifically tailored for Intel Gaudi devices. This plugin extends the base UCX functionality with Gaudi-specific optimizations and transport preferences.

## Features

- **Gaudi Memory Detection**: Automatic detection of Gaudi device memory vs host memory
- **Transport Optimization**: Preference for Gaudi-specific UCX transports when available
- **Device Context Management**: Proper handling of multiple Gaudi devices
- **Optimized Transfer Paths**: Direct Gaudi-to-Gaudi memory transfers when possible

## Build Requirements

- UCX 1.18.0 or later with Gaudi transport support
- Intel Gaudi SDK and drivers
- CUDA toolkit (optional, for mixed GPU workloads)

## Build Configuration

### Dynamic Plugin (Default)

```bash
meson setup build
cd build
ninja
```

The UCX Gaudi plugin will be built as a shared library and loaded dynamically.

### Static Plugin

```bash
meson setup build -Dstatic_plugins=ucx_gaudi
cd build  
ninja
```

### Combined with Other Plugins

```bash
meson setup build -Dstatic_plugins=ucx,ucx_gaudi,posix
```

## Configuration Options

The UCX Gaudi backend supports the following configuration parameters:

- `gaudi_optimize`: Enable/disable Gaudi-specific optimizations (default: true)
- `gaudi_transport`: Preferred Gaudi transport name (default: "gaudi")
- `ucx_devices`: UCX device selection string
- `num_workers`: Number of UCX workers (default: 1)

### Example Configuration

```cpp
nixlAgentConfig config;
config.backends = {"UCX_GAUDI"};
config.backend_params["UCX_GAUDI"] = {
    {"gaudi_optimize", "true"},
    {"gaudi_transport", "gaudi"},
    {"num_workers", "2"}
};

nixlAgent agent("gaudi_agent", config);
```

## Usage

The UCX Gaudi plugin is used automatically when:

1. The backend is specified as "UCX_GAUDI" in the agent configuration
2. The plugin detects Gaudi devices in the system
3. Memory transfers involve Gaudi device memory

### Memory Types Supported

- **DRAM_SEG**: Host memory
- **VRAM_SEG**: Gaudi device memory

## Performance Considerations

- **Gaudi-to-Gaudi Transfers**: When both source and destination are on Gaudi devices, the plugin uses optimized direct transfer paths
- **Mixed Transfers**: Host-to-Gaudi and Gaudi-to-Host transfers use appropriate staging strategies
- **Transport Selection**: The plugin configures UCX to prefer Gaudi transports: `"gaudi,rc_verbs,ud_verbs,rc_mlx5,ud_mlx5,tcp"`

## Debugging

Enable debug logging to see Gaudi-specific optimization decisions:

```bash
export NIXL_LOG_LEVEL=DEBUG
```

Debug messages will show:
- Gaudi device detection
- Transport selection
- Optimization path decisions
- Memory type identification

## Limitations

- Requires UCX built with Gaudi transport support
- Currently supports Intel Gaudi devices only
- Some optimizations are placeholder implementations that require full Gaudi SDK integration

## Future Enhancements

- Full Gaudi SDK integration for memory detection
- Advanced memory mapping optimizations  
- Multi-device topology awareness
- Direct RDMA over Gaudi fabric support