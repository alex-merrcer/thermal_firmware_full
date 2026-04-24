#ifndef PAGE_REGISTRY_H
#define PAGE_REGISTRY_H

#include "redpic1_app.h"
#include "ui_manager.h"

/*
 * 页面注册表负责维护只读的页面回调表，以及 UI 返回导航依赖的父页面关系。
 * 页面私有状态全部保留在 page_registry.c 内部，对外只暴露这一层公共契约。
 */

/* 根据页面编号返回对应的回调表，编号非法时返回 0。 */
const ui_page_ops_t *page_registry_get_ops(ui_page_id_t page_id);
/* 返回指定页面的逻辑父页面，供 ui_manager_navigate_back() 使用。 */
ui_page_id_t page_registry_get_parent(ui_page_id_t page_id);
/* 将异步服务响应回流给页面内部状态机处理。 */
void page_registry_on_service_response(const app_service_rsp_t *rsp);

#endif
