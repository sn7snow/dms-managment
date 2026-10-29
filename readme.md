# Drawing File Management System 产品说明

## 1. 产品概述

Drawing File Management System（DMS）用于在局域网内集中管理图纸和其他文件。Linux 服务端保存文件、版本、权限和审计信息；Windows Qt 客户端提供浏览、上传、下载、编辑锁和版本管理界面。

本说明覆盖功能、用户操作、权限逻辑、主要技术实现、配置和维护。当前部署目录为 `/opt/dms`，Qt 客户端可以在 Windows 或 Linux 桌面运行。

## 2. 功能一览

- **账号登录**：服务端验证账号和 PBKDF2-HMAC-SHA256 密码摘要；客户端不保存密码或会话令牌。
- **目录浏览**：按权限过滤目录和文件，目录采用展开时加载。
- **属性列表**：显示名称、修改日期、类型、大小、创建者、权限、版本、锁定用户和锁定时间；列宽可拖动调整，列也可拖动换序。日期格式为 `年-月-日 时:分:秒`，文件大小自动选择单位，权限按“读、写、删、手动增版、手动删版、上传”显示为六位 0/1 字符串，版本列使用 `vN`。没有系统消息时，消息面板会显示权限说明、双击打开/锁定与刷新提示、问题反馈邮箱。
- **上传本地文件或文件夹**：可选择本机文件或文件夹上传到当前目录；如果选中的是文件，则目标为该文件所在目录。文件夹结构会递归创建。上传权限单独控制，不要求先打开文件或取得编辑锁。
- **图纸编辑**：打开文件时先向服务端申请编辑锁，再下载到本地临时目录并调用系统关联程序。没有获得锁时按只读方式打开。
- **保存编辑内容**：CAD 程序保存到本地临时文件后，在客户端选择“保存当前编辑内容”，将本地修改上传回同一服务器路径。
- **下载文件**：将服务器文件或指定历史版本保存到用户选择的本地路径。
- **版本管理**：文件上传/保存产生版本快照；可查看版本、下载旧版本、手动新增版本，以及按权限删除非最新版本。
- **目录与文件管理**：按授权新建目录、重命名/移动文件或目录、删除文件；目录删除走审批流程。
- **占用锁提示**：名称列显示红色锁图标，悬停可查看占用者。
- **请求编辑和临时消息**：右击文件可请求当前编辑者释放文件；文件释放和编辑请求会实时通知已登录客户端。消息不写入磁盘，只在界面固定三行区域中显示，每两秒显示一条，最多保留最近三条。

## 3. 用户操作逻辑

### 3.1 连接和登录

1. 启动 `dms-qt-client`。
2. 客户端默认读取程序目录中的 `server-connection.json`；也可以在服务器地址栏直接修改地址。
3. 确认 CA 已安装到 Windows 信任存储，且地址中的主机名/IP 与服务器证书 SAN 匹配。
4. 输入用户名和密码并登录。修改地址后，重新连接即可使用新服务器。

### 3.2 上传本地文件

1. 选中目标目录，或选中目标文件（此时目标目录为该文件的父目录）。
2. 点击“上传本地文件”或右键选择“上传本地文件到此目录…”。
3. 在本机文件选择框中选择文件。
4. 服务端根据 `permissions.csv` 的“上传”权限检查目标路径。成功后文件写入服务器、生成版本快照并刷新列表。

文件重名时，服务端以新上传内容替换当前文件内容，同时创建新版本。文件被其他用户持有未过期编辑锁时，上传会被拒绝，避免覆盖正在编辑的内容。

选择文件夹时，客户端先递归创建远端目录，再逐个上传文件；每个目录和文件仍由服务端分别执行上传权限检查。遇到无法读取的本地文件或服务端拒绝时，上传会中断并显示错误。

### 3.3 编辑和锁

1. 双击可读文件，客户端调用 `/api/file/open` 申请锁。
2. 服务端允许申请时，客户端下载文件到本机临时目录并打开；其他用户若已持有该文件锁，则以只读方式打开。
3. 编辑者在 CAD 应用中保存文件后，回到客户端点击“保存当前编辑内容”。这与“上传本地文件”是不同操作：前者保存已经打开的图纸，后者从任意本地路径导入文件。
4. 关闭文件时通过 `/api/file/close` 释放锁；客户端也会定期发送 heartbeat。

锁采用 30 分钟滑动租约：服务端连续 30 分钟未收到该客户端的 heartbeat 才会清除锁，后台每约 5 秒检查一次。Qt 客户端对仍登记在“已打开文件”列表中的可编辑文件每 5 分钟续约一次。因此，外部 CAD 程序关闭文件本身不会停止续约；请在客户端右击该文件并选择“关闭文件并释放占用锁”。客户端崩溃、退出或网络中断导致心跳停止时，锁会在最后一次心跳后约 30 分钟过期。锁定时间按 Asia/Shanghai（UTC+08:00）显示。

右击文件选择“请求编辑”后，其他已登录客户端会看到“用户xxx想要编辑'文件路径'，请求释放。”；文件锁正常释放或过期时会显示“文件路径已经释放。”。消息只保存在服务端内存中的在线会话队列，退出登录后不保留。

### 3.4 删除和审批

- 文件删除按权限直接执行。
- 目录删除提交审批请求；目录会先对用户隐藏，之后由审批任务处理。
- 审批任务每日运行一次，也可以按部署者维护的计划任务执行。
- 审批队列位于 `/opt/dms/admin/pending_delete.csv`，在文件库之外，Qt 客户端不能浏览或编辑。四列依次为提交用户、逻辑路径、状态、请求时间；`0` 表示待处理、`1` 表示批准、`2` 表示拒绝。只应修改真实待处理记录的状态，不应伪造记录或改写路径；批准目录会被递归删除。cron 会拒绝删除文件库根目录。
- 队列属于高权限控制数据，仅授权给受信任的系统操作者。不要通过 Samba 或文件 API 共享写入；编辑时应确保 cron 任务没有运行，完成后再恢复定时器。需立即处理时可运行 `systemctl --user start dms-cron.service`。

## 4. 用户、组和权限

### 4.1 用户与组

运行配置 `/opt/dms/etc/config.json` 保存用户、PBKDF2 salt/hash、所属组和 `admin` 标志。该标志目前仅作为配置元数据读取，不会自动授予文件权限、审批权限或用户管理权限；实际文件操作仍由 `permissions.csv` 决定。当前默认示例分组为：

| 组 | 成员 |
|---|---|
| `g1` | `u01`、`u02` |
| `g2` | `u03`、`u04` |
| `g3` | `u05`、`u06` |
| `g4` | `u07`、`u08` |
| `g5` | `u09`、`u10` |

实际用户清单以服务器 `config.json` 为准。账户由系统操作者添加；Qt 客户端没有用户管理功能，应用内的 `admin` 用户也不能新增账户。

#### 系统操作者新增用户

1. 在服务器受信任的交互式终端运行 `/opt/dms/bin/dms-server --hash-password`，按提示输入新用户密码。命令输出 `pass_salt` 和 `pass_hash`；不要把密码作为命令行参数，也不要保存明文密码。
2. 备份 `/opt/dms/etc/config.json`，在 `users` 数组中加入一个账户对象。把哈希工具输出的盐值和哈希原样填入，设置唯一的 `user`、所属 `groups`，并通常设为 `"admin": false`：

   ```json
   {
     "user": "newuser",
     "pass_salt": "<命令输出的 pass_salt>",
     "pass_hash": "<命令输出的 pass_hash>",
     "groups": ["g1"],
     "admin": false
   }
   ```

3. 在 `/opt/dms/etc/permissions.csv` 中为该组或用户配置必要的最小权限。
4. 检查 JSON/CSV 格式后重启服务：`systemctl --user restart dms-server`。账户在服务启动时加载；新用户应重新登录客户端。

配置包含口令摘要和服务内部 refresh token，应限制为系统操作者可读写。把账户加入名为 `admin` 的组或将字段设为 `true`，都不会自动授予超级用户权限。

### 4.2 `permissions.csv`

服务器运行权限表为 `/opt/dms/etc/permissions.csv`。当前 9 列如下：

| 列 | 含义 |
|---|---|
| 路径 | 逻辑文件/目录路径；以 `/` 结尾表示目录继承规则 |
| 对象(部门/用户) | 用户名、组名或 `@all` |
| 读 | 列出/读取对象 |
| 写 | 编辑已存在文件或创建目录等写操作 |
| 删 | 删除文件或提交目录删除 |
| 手动增版 | 手动创建版本 |
| 手动删版 | 删除非最新版本 |
| 保存策略 | `0` 覆盖当前版本，`1` 创建新版本 |
| 上传 | 从用户本机导入文件；不隐含编辑权限 |

权限值为 `1` 授权，`0` 不授权。路径规则沿父目录继承；同一对象以最具体匹配路径为准；同一优先级以及用户所属多个组的授权按并集计算。由于规则采用授权并集，`@all` 的授权会与用户/组授权相加；不要把 `@all` 的写/上传授权用于需要组隔离的目录。

建议对组目录明确加入 `@all` 的全拒绝行，再为该目录的组和成员添加允许行。例如：

```csv
/g1/,@all,0,0,0,0,0,0,0
/g1/,g1,1,1,1,1,1,1,1
/g1/,u01,1,1,1,1,1,1,1
/g1/,u02,1,1,1,1,1,1,1
```

服务器会监测权限文件修改并重新加载。修改前建议备份，且确保 CSV 的字段数和每项权限值有效。

### 4.3 管理员权限说明

`config.json` 中 `"admin": false` 表示该账户没有被配置标记为管理员；当前代码不会依据此字段授予或撤销操作权限。`admin: true` 同样只是账户元数据，不会自动绕过 `permissions.csv`，也不提供客户端用户管理或审批功能。账户在 Qt 客户端可访问的文件及操作完全由权限 CSV 决定。

当前部署的 `/,admin,1,0,0,0,0,0,0` 规则使名为 `admin` 的账号沿路径继承读取权限，但不授予写、删、手动增版、手动删版或上传权限。因此它可在客户端浏览其有读权限的文件，但不能通过应用添加用户、编辑审批队列或执行敏感系统操作。账户添加和审批队列维护由系统操作者在服务器侧处理。

## 5. 连接配置与 IP/端口变更

### 5.1 Qt 客户端连接文件

连接参数保存在英文命名的 JSON 文件 `server-connection.json`：

- 项目模板：[`etc/server-connection.json`](./etc/server-connection.json)
- Linux 部署目录：`/opt/dms/etc/server-connection.json`
- Windows Qt 客户端：将该文件复制到 `dms-qt-client.exe` 同一目录。CMake 构建时也会把模板复制到构建输出目录。

配置示例：

```json
{
  "server": {
    "scheme": "https",
    "host": "192.168.1.120",
    "port": 8080
  }
}
```

服务器 IP 变化后，只需把 `host` 改为新地址；端口变化时改 `port`。客户端启动后会以此文件为默认连接地址，无需因地址变化重写或重新编译代码。界面地址栏仍可临时覆盖该地址。也可用以下方式指定其他配置文件：

```powershell
.\dms-qt-client.exe --connection-config "C:\DMS\server-connection.json"
```

### 5.2 服务端监听和网络安全

服务端监听参数在 `/opt/dms/etc/config.json` 的 `listen_address` 和 `port`：

- `listen_address: "0.0.0.0"` 表示监听所有网络接口，不是客户端要连接的 IP。
- `port` 是服务端端口，应与客户端连接 JSON 中的端口保持一致。
- TLS 证书和密钥路径由 `tls_cert_file` 与 `tls_key_file` 配置。
- `tls_ca_file` 是可选的 CA 证书路径，供 `dms-cron` 验证本机 HTTPS 服务端证书。当前部署配置为 `/opt/dms/etc/tls/ca.crt`，证书由 `/home/snow07/rootCA.crt` 对应的本地 CA 签发。该文件应是 CA 公钥证书，不是服务器私钥，也不是服务器 leaf 证书。

局域网部署时，除更新客户端连接 JSON 外，还要确认服务器端口可达、Windows/WSL 或 Linux 防火墙允许该端口、证书 SAN 覆盖客户端使用的主机名/IP，并且客户端信任签发 CA。不要为解决证书问题关闭 TLS 校验。

`server-connection.json` 只保存地址，不保存密码或 token；可以分发给客户端。`config.json` 包含密码摘要与内部 refresh token，应保持受限权限，不要分发或提交到版本库。

### 5.3 Cron、内部刷新和 CA 信任

`dms-server` 与 `dms-cron` 是两个独立进程。每日 cron 任务处理 `/opt/dms/admin/pending_delete.csv` 中已批准或拒绝的目录删除记录后，需要通知正在运行的服务端重新加载审批状态和文件索引。cron 因而会连接到 `https://127.0.0.1:<port>/api/internal/refresh`，并在请求头中附带 `config.json` 的 `refresh_token`。服务端仅在 token 匹配时接受刷新请求。

即使目标是本机回环地址，这仍是网络连接，而不是进程内函数调用。cron 会验证服务端证书的信任链，并确认 leaf 证书覆盖 `127.0.0.1`。这样可以避免在端口被其他程序占用、服务配置异常或本机通信被干扰时，把内部 refresh token 交给冒充 DMS 服务端的程序；TLS 同时保护请求内容在传输中的机密性和完整性。不要通过关闭 `verify_peer` 或忽略证书错误来规避验证失败。

#### `tls_ca_file` 的路径和用途

- 路径是**运行 `dms-cron` 的 Linux 服务器本地文件系统路径**，不是 Qt 客户端配置，也不是证书文件夹。
- 文件内容应是签发 `tls_cert_file` 所配置服务端证书的 CA 证书（可公开分发的证书），不应是 `tls_key_file` 中的私钥。
- 当前示例配置为：

  ```json
  {
    "tls_cert_file": "/opt/dms/etc/tls/server.crt",
    "tls_key_file": "/opt/dms/etc/tls/server.key",
    "tls_ca_file": "/opt/dms/etc/tls/ca.crt"
  }
  ```

- cron 优先使用 `tls_ca_file` 加载信任 CA；未配置时会回退尝试使用 `tls_cert_file`。若服务端证书由独立的本地 CA 签发，通常应明确配置 CA 路径，否则 cron 可能因证书链无法验证而失败。
- `tls_ca_file` 只用于 cron 到本机服务端的验证。Windows Qt 客户端仍需在 Windows 信任存储中信任签发服务器证书的 CA；修改服务器上的这个路径不会自动更新客户端的信任。
- 当前服务端证书的 SAN 需包括 cron 连接用的 `127.0.0.1`，同时也要包括 Qt 客户端实际访问的 IP 或 DNS 名称。

#### 更换证书的步骤

**只更换服务端证书，CA 不变：**

1. 用现有 CA 为服务端签发新证书，并生成对应私钥。确保证书 SAN 同时包含 `127.0.0.1` 和客户端使用的服务器地址。
2. 将新文件安全放到服务器，更新 `/opt/dms/etc/config.json` 中的 `tls_cert_file` 和 `tls_key_file`。私钥应限制读取权限，不得分发到客户端。
3. 先备份配置和旧证书，再重启服务并检查状态：

   ```sh
   systemctl --user restart dms-server
   systemctl --user status dms-server
   ```

4. 手动运行一次 cron，验证内部 TLS 请求成功：

   ```sh
   systemctl --user start dms-cron.service
   systemctl --user status dms-cron.service
   ```

   成功时 oneshot 服务会显示 `status=0/SUCCESS`（随后通常回到 `inactive (dead)`），这表示任务已经正常结束。
5. 确认 Qt 客户端仍信任该 CA，并能通过服务器地址连接。

CA 没有变化时，`tls_ca_file` 通常不需要更换。

**更换签发 CA：**

1. 将新 CA 的公钥证书放入服务器受限维护目录，例如 `/opt/dms/etc/tls/ca.crt`，并把 `config.json` 的 `tls_ca_file` 更新为其绝对路径。
2. 使用新 CA 签发新的服务端证书，并按上述步骤更新 `tls_cert_file`、`tls_key_file`。确保证书 SAN 覆盖回环地址和客户端地址。
3. 把新的 CA 证书安装到每台 Windows 客户端的受信任根证书存储；服务器上的 `tls_ca_file` 不会替客户端完成此步骤。
4. 重启 `dms-server`，运行一次 `dms-cron.service`，再分别验证 Qt 客户端连接和 cron 任务成功。
5. 若需过渡期同时支持新旧 CA，应先让相关验证端信任新 CA，再切换服务端证书；确认所有客户端已更新信任后，再移除旧 CA 信任。

CA 证书是公钥证书，可以分发给客户端；服务端私钥必须保密。证书文件更新后应保留回滚所需的旧证书和配置备份，并在整个切换期间保持 TLS 校验开启。

## 6. 技术实现

### 6.1 组件

- **DMS Server**：C++20、Boost.Asio/Beast、OpenSSL、nlohmann-json，提供 HTTPS API、认证、权限检查、文件存储、版本和锁。
- **DMS Cron**：处理待审批删除记录，并通过 loopback HTTPS 请求通知服务端刷新索引。
- **Qt Client**：Qt 6 Widgets + Network，调用 API、展示文件树、管理本地文件选择和编辑流程。

### 6.2 文件和版本存储

- 当前文件存放在 `/opt/dms/filestore/<逻辑路径>`。
- 版本存放在 `/opt/dms/version_store/<逻辑父目录>/<文件名>/rev_N.<扩展名>`。
- `/opt/dms/etc/file_metadata.json` 记录文件创建者元数据。
- 审计日志写入 `/opt/dms/logs`。
- 待审批删除记录位于文件库之外的 `/opt/dms/admin/pending_delete.csv`，只应授权给受信任的系统操作者。获准编辑者可批准递归删除目录。

## 7. 构建、部署与运行

### 7.1 Linux Server

Ubuntu/Debian 安装依赖并构建：

```sh
sudo apt-get install build-essential cmake libboost-system-dev libssl-dev nlohmann-json3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/dms
```

部署配置文件时不要覆盖现有生产配置。修改 server `config.json` 后重启服务：

```sh
systemctl --user restart dms-server
systemctl --user status dms-server
```

当前部署使用 user systemd service。若切换到系统级服务或独立服务账号，应同步检查 `/opt/dms` 所有权、TLS 私钥读取权限和服务启动参数。

### 7.2 Windows Qt Client

使用安装了 Qt 6.4+ 与匹配 MSVC kit 的 Developer PowerShell：

```powershell
cmake -S qt-client -B qt-client/build `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.x.x\msvc2022_64"
cmake --build qt-client/build --config Release
```

Qt build 输出目录会带有 `server-connection.json`。发布时将该 JSON 与 exe 一起分发，并确保配置中的 TLS 地址与证书一致。

### 7.3 备份建议

定期备份 `config.json`、`permissions.csv`、`/opt/dms/admin/pending_delete.csv`、TLS 证书/CA、TLS 私钥、`filestore` 和 `version_store`。备份包含敏感凭据的配置时，应限制访问并采用加密存储。恢复时先停止服务，再恢复数据和配置，并检查文件所有者与权限。

## 8. 代码入口索引

- 服务端 API、锁状态、heartbeat 和上传逻辑：[`src/server_main.cpp`](./src/server_main.cpp)
- 文件锁分片锁实现与存储索引类型：[`include/dms/storage.hpp`](./include/dms/storage.hpp)、[`src/storage.cpp`](./src/storage.cpp)
- CSV 权限规则解析和匹配：[`include/dms/permission_provider.hpp`](./include/dms/permission_provider.hpp)、[`src/permission_provider.cpp`](./src/permission_provider.cpp)
- Qt 客户端事件、上传、列表和锁图标：[`qt-client/src/main_window.cpp`](./qt-client/src/main_window.cpp)、[`qt-client/src/main_window.hpp`](./qt-client/src/main_window.hpp)
- 连接配置模板：[`etc/server-connection.json`](./etc/server-connection.json)
