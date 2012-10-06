package net.rubygrapefruit.platform;

import java.util.List;

/**
 * Provides access to the file systems of the current machine.
 */
@ThreadSafe
public interface FileSystems extends NativeIntegration {
    /**
     * Returns the set of all file systems for the current machine.
     *
     * @return The set of file systems. Never returns null.
     * @throws NativeException On failure.
     */
    @ThreadSafe
    List<FileSystem> getFileSystems() throws NativeException;
}
