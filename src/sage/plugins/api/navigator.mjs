const GlobalNavigator = globalThis.Navigator
export const Navigator =
  typeof GlobalNavigator === 'function'
    ? GlobalNavigator
    : class Navigator {
        constructor() {
          const n = globalThis.navigator
          if (n && typeof n === 'object') {
            this.appName = String(n.appName || '')
            this.appVersion = String(n.appVersion || '')
            this.userAgent = String(n.userAgent || '')
          } else {
            this.appName = ''
            this.appVersion = ''
            this.userAgent = ''
          }
        }
      }

const navigator = globalThis.navigator
export default navigator && typeof navigator === 'object' ? navigator : new Navigator()
