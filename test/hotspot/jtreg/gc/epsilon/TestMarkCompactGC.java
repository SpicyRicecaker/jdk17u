/*
 * Copyright (c) 2017, 2018, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package gc.epsilon;

/**
 * @test TestMarkCompactGC
 * @key randomness
 * @requires vm.gc.Epsilon & !vm.graal.enabled
 * @summary Epsilon MarkCompact GC works
 * @library /test/lib
 * @run main/othervm -Xmx512m -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC -XX:+EpsilonMarkCompactGC -XX:+EpsilonVerify                      gc.epsilon.TestMarkCompactGC
 * @run main/othervm -Xmx512m -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC -XX:+EpsilonMarkCompactGC                                         gc.epsilon.TestMarkCompactGC
 * @run main/othervm -Xmx512m -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC -XX:+EpsilonMarkCompactGC -XX:+EpsilonVerify -XX:+EpsilonUncommit gc.epsilon.TestMarkCompactGC
 * @run main/othervm -Xmx512m -XX:+UnlockExperimentalVMOptions -XX:+UseEpsilonGC -XX:+EpsilonMarkCompactGC                    -XX:+EpsilonUncommit gc.epsilon.TestMarkCompactGC
 */

import java.util.concurrent.*;
import java.util.Random;

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.Utils;

public class TestMarkCompactGC {
    // 10 million
    // .24 GB allocation?
    static int SIZE = Integer.getInteger("size", 10_000_000);
    // 100 million
    static int COUNT = Integer.getInteger("count", 100_000_000); // ~2.4 GB allocation

    static Object[] sink;

    public static void main(String... args) {
        System.out.println("ok it's modified");
        // random thread
        Random r = Utils.getRandomInstance();
        // object array of 10 million
        sink = new Object[SIZE];
        // for each 100 million
        // this should in theory generate 90 million garbage objects
        // since the heap size is only 512 mb, and allocation of 100 million objects takes 2.4 gb,
        // we should collect garbage quite a few times
        for (int c = 0; c < COUNT; c++) {
            // set the sink to some random number
            sink[r.nextInt(SIZE)] = new Object();
        }
    }
}