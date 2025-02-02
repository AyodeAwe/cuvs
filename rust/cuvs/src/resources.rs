/*
 * Copyright (c) 2024, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

use crate::error::{check_cuvs, Result};
use std::io::{stderr, Write};

#[derive(Debug)]
pub struct Resources(pub ffi::cuvsResources_t);

impl Resources {
    pub fn new() -> Result<Resources> {
        let mut res: ffi::cuvsResources_t = 0;
        unsafe {
            check_cuvs(ffi::cuvsResourcesCreate(&mut res))?;
        }
        Ok(Resources(res))
    }
}

impl Drop for Resources {
    fn drop(&mut self) {
        unsafe {
            if let Err(e) = check_cuvs(ffi::cuvsResourcesDestroy(self.0)) {
                write!(stderr(), "failed to call cuvsResourcesDestroy {:?}", e)
                    .expect("failed to write to stderr");
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_resources_create() {
        let _ = Resources::new();
    }
}
