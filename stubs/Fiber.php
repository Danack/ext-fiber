<?php

final class Fiber
{
    /**
     * @param callable $callback Function to invoke when starting the Fiber.
     * @param mixed ...$args Function arguments.
     */
    public static function run(callable $callback, mixed ...$args): void { }

    /**
     * Private constructor to force use of {@see create()}.
     */
    private function __construct() { }

    /**
     * Suspend execution of the fiber until the given awaitable resolves.
     *
     * @param Awaitable $awaitable
     *
     * @return mixed Value given to {@see Fiber::resume()}.
     *
     * @throws FiberError Thrown if not within a fiber context.
     */
    public static function await(Awaitable $awaitable): mixed { }

    /**
     * Returns the current Fiber context or null if not within a fiber.
     *
     * @return bool True if currently executing within a fiber context, false if in root context.
     */
    public static function inFiber(): bool { }
}
