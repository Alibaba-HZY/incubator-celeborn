/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.spark.celeborn

import org.apache.spark.shuffle.FetchFailedException

import org.apache.celeborn.common.util.ExceptionMaker

object ExceptionMakerHelper {

  val FETCH_FAILURE_ERROR_MSG = "Celeborn FetchFailure with appShuffleId/shuffleId: "

  val SHUFFLE_FETCH_FAILURE_EXCEPTION_MAKER = new ExceptionMaker() {
    override def makeFetchFailureException(
        appShuffleId: Int,
        shuffleId: Int,
        partitionId: Int,
        e: Exception): Exception = {
      new FetchFailedException(
        null,
        appShuffleId,
        -1,
        partitionId,
        FETCH_FAILURE_ERROR_MSG + appShuffleId + "/" + shuffleId,
        e)
    }
  }
}
