fn main() {
    #[cfg(not(any(target_os = "ios", target_os = "android")))]
    hikari::main();
}
