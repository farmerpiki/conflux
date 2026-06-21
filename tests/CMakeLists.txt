if(CONFLUX_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    include("${CMAKE_CURRENT_LIST_DIR}/TestHelpers.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/ApiSurfaceImportTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/ApiSurfaceCompileFailTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/MainTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/ExternalTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/CoreTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpCoreTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpFacadeTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpPolicyTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpObservabilityTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpAuthTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpResponseTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpJsonTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpServerHelpersTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpParseHelpersTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpStaticCoreTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpRouterMatchTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpRouterDispatchTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpVhostTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpProxyTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpServerTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpCompressionTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/HttpLifecycleTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/TemplateProcessTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/JsonTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/SocketTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/WorkTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/NetworkSurfaceTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/DnsSurfaceTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/IoTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/DbTests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/E2ETests.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/TestDiscovery.cmake")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/BuildAndDocsChecks.cmake")

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_PACKAGE_SMOKE_PREFIX)
    conflux_add_package_config_install_tree_test(
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_BINARY_DIR}/package-smoke")
endif()

if((CONFLUX_BUILD_TESTS OR CONFLUX_BUILD_PACKAGE_TESTS)
        AND CONFLUX_RUN_INSTALL_TREE_SMOKE)
    conflux_add_install_tree_smoke_test("${PROJECT_SOURCE_DIR}")
endif()
