<?php

interface Awaitable
{
    /**
     * @param callable(?\Throwable $exception, mixed $value):void $onResolve
     */
    public function onResolve(callable $onResolve): void;
}
