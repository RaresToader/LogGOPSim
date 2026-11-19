namespace goalx {

template <typename T>
struct TimedComputation {
    T value;
    double seconds;
};

template <>
struct TimedComputation<void> {
    double seconds;
};

template <typename F, typename... Args>
[[nodiscard]] auto timed(F&& f, Args&&... args) {
    using clock = std::chrono::steady_clock;
    using Result = std::invoke_result_t<F, Args...>;

    clock::time_point start = clock::now();
    const auto elapsed = [&start] {
        return std::chrono::duration<double>(clock::now() - start).count();
    };

    if constexpr (std::is_void_v<Result>) {
        std::invoke(std::forward<F>(f), std::forward<Args>(args)...);
        return TimedComputation<void>{elapsed()};
    } else {
        return TimedComputation<Result>{
            std::invoke(std::forward<F>(f), std::forward<Args>(args)...), elapsed()};
    }
}

} // namespace goalx