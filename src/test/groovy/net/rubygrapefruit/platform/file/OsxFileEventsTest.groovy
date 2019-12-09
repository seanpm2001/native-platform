/*
 * Copyright 2012 Adam Murdoch
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

package net.rubygrapefruit.platform.file

import net.rubygrapefruit.platform.Native
import net.rubygrapefruit.platform.internal.Platform
import net.rubygrapefruit.platform.internal.jni.OsxFileEventFunctions
import org.junit.Rule
import org.junit.rules.TemporaryFolder
import spock.lang.Requires
import spock.lang.Specification
import spock.util.concurrent.AsyncConditions

@Requires({ Platform.current().macOs })
class OsxFileEventsTest extends Specification {
    @Rule
    TemporaryFolder tmpDir
    final OsxFileEventFunctions fileEvents = Native.get(OsxFileEventFunctions.class)
    FileWatcher watcher
    def callback = new TestCallback()
    File dir

    def setup() {
        dir = tmpDir.newFolder()
    }

    def cleanup() {
        stopWatcher()
    }

    def "caches file events instance"() {
        expect:
        Native.get(OsxFileEventFunctions.class) is fileEvents
    }

    def "can open and close watcher on a directory without receiving any events"() {
        when:
        startWatcher(dir)

        then:
        0 * _
    }

    def "can open and close watcher on a directory receiving an event"() {
        given:
        startWatcher(dir)

        when:
        def expectedChanges = expectThat pathChanged(dir)
        new File(dir, "a.txt").createNewFile()

        then:
        expectedChanges.await()
    }

    def "can open and close watcher on a directory receiving multiple events"() {
        given:
        def latency = 0.3
        startWatcher(latency, dir)

        when:
        def expectedChanges = expectThat pathChanged(dir)
        new File(dir, "a.txt").createNewFile()

        then:
        expectedChanges.await()

        when:
        expectedChanges = expectThat pathChanged(dir)
        Thread.sleep((long) (latency * 1000 + 100))
        new File(dir, "b.txt").createNewFile()

        then:
        expectedChanges.await()
    }

    def "can open and close watcher on multiple directories receiving multiple events"() {
        given:
        def latency = 0.3
        def dir2 = tmpDir.newFolder()

        println "-> $dir"
        println "-> $dir2"

        startWatcher(latency, dir2, dir)

        when:
        def expectedChanges = expectThat pathChanged(dir)
        new File(dir, "a.txt").createNewFile()

        then:
        expectedChanges.await()

        when:
        expectedChanges = expectThat pathChanged(dir2)
        new File(dir2, "b.txt").createNewFile()

        then:
        expectedChanges.await()
    }

    def "can be started once and stopped multiple times"() {
        given:
        startWatcher(dir)

        when:
        watcher.close()
        watcher.close()

        then:
        noExceptionThrown()
    }

    def "can be used multiple times"() {
        given:
        startWatcher(dir)

        when:
        def expectedChanges = expectThat pathChanged(dir)
        new File(dir, "a.txt").createNewFile()

        then:
        expectedChanges.await()
        stopWatcher()

        when:
        startWatcher(dir)
        expectedChanges = expectThat pathChanged(dir)
        new File(dir, "b.txt").createNewFile()

        then:
        expectedChanges.await()
    }

    private void startWatcher(double latency = 0.3, File... roots) {
        watcher = fileEvents.startWatching(roots*.absolutePath.toList(), latency, callback)
    }
    private void stopWatcher() {
        watcher?.close()
    }

    private AsyncConditions expectThat(FileWatcherCallback delegateCallback) {
        return callback.expectCallback(delegateCallback)
    }

    private FileWatcherCallback pathChanged(File path) {
        return { changedPath ->
            assert changedPath == path.canonicalPath + "/"
        }
    }

    private static class TestCallback implements FileWatcherCallback {
        private AsyncConditions conds
        private FileWatcherCallback delegateCallback

        AsyncConditions expectCallback(FileWatcherCallback delegateCallback) {
            this.conds = new AsyncConditions()
            this.delegateCallback = delegateCallback
            return conds
        }

        @Override
        void pathChanged(String path) {
            assert conds != null
            conds.evaluate {
                delegateCallback.pathChanged(path)
            }
            conds = null
            delegateCallback = null
        }
    }
}
