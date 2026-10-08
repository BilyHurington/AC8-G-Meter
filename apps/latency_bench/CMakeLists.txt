add_executable(latency_bench main.cpp)
target_link_libraries(latency_bench PRIVATE ft_sources ft_core)
ft_warnings(latency_bench)
