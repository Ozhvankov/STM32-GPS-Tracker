/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    usb.c
 * @brief   USB Driver code.
 *
 * @addtogroup USB
 * @{
 */

#include <string.h>

#include "ch.h"
#include "hal.h"
#include "usb.h"

#if HAL_USE_USB || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local variables.                                                   */
/*===========================================================================*/

static const uint8_t zero_status[] = {0x00, 0x00};
static const uint8_t active_status[] ={0x00, 0x00};
static const uint8_t halted_status[] = {0x01, 0x00};

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief  SET ADDRESS transaction callback.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 */
void set_address(USBDriver *usbp) {

  usbp->address = usbp->setup[2];
  usb_lld_set_address(usbp);
  if (usbp->config->event_cb)
    usbp->config->event_cb(usbp, USB_EVENT_ADDRESS);
  usbp->state = USB_SELECTED;
}

/**
 * @brief   Standard requests handler.
 * @details This is the standard requests default handler, most standard
 *          requests are handled here, the user can override the standard
 *          handling using the @p requests_hook_cb hook in the
 *          @p USBConfig structure.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @return              The request handling exit code.
 * @retval FALSE        Request not recognized by the handler or error.
 * @retval TRUE         Request handled.
 */
static bool_t default_handler(USBDriver *usbp) {
  const USBDescriptor *dp;

  /* Decoding the request.*/
  switch (((usbp->setup[0] & (USB_RTYPE_RECIPIENT_MASK |
                              USB_RTYPE_TYPE_MASK)) |
           (usbp->setup[1] << 8))) {
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_GET_STATUS << 8):
    /* Just returns the current status word.*/
    usbSetupTransfer(usbp, (uint8_t *)&usbp->status, 2);
    return TRUE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_CLEAR_FEATURE << 8):
    /* Only the DEVICE_REMOTE_WAKEUP is handled here, any other feature
       number is handled as an error.*/
    if (usbp->setup[2] == USB_FEATURE_DEVICE_REMOTE_WAKEUP) {
      usbp->status &= ~2;
      usbSetupTransfer(usbp, NULL, 0);
      return TRUE;
    }
    return FALSE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_SET_FEATURE << 8):
    /* Only the DEVICE_REMOTE_WAKEUP is handled here, any other feature
       number is handled as an error.*/
    if (usbp->setup[2] == USB_FEATURE_DEVICE_REMOTE_WAKEUP) {
      usbp->status |= 2;
      usbSetupTransfer(usbp, NULL, 0);
      return TRUE;
    }
    return FALSE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_SET_ADDRESS << 8):
    /* The SET_ADDRESS handling can be performed here or postponed after
       the status packed depending on the USB_SET_ADDRESS_MODE low
       driver setting.*/
#if USB_SET_ADDRESS_MODE == USB_EARLY_SET_ADDRESS
    if ((usbp->setup[0] == USB_RTYPE_RECIPIENT_DEVICE) &&
        (usbp->setup[1] == USB_REQ_SET_ADDRESS))
      set_address(usbp);
#endif
    usbSetupTransfer(usbp, NULL, 0);
    return TRUE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_GET_DESCRIPTOR << 8):
    /* Handling descriptor requests from the host.*/
    dp = usbp->config->get_descriptor_cb(
           usbp, usbp->setup[3], usbp->setup[2],
           usb_lld_fetch_word(&usbp->setup[4]));
    if (dp == NULL)
      return FALSE;
    usbSetupTransfer(usbp, (uint8_t *)dp->ud_string, dp->ud_size);
    return TRUE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_GET_CONFIGURATION << 8):
    /* Returning the last selected configuration.*/
    usbSetupTransfer(usbp, &usbp->configuration, 1);
    return TRUE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_SET_CONFIGURATION << 8):
    /* Handling configuration selection from the host.*/
    usbp->configuration = usbp->setup[2];
    if (usbp->configuration == 0)
      usbp->state = USB_SELECTED;
    else
      usbp->state = USB_ACTIVE;
    if (usbp->config->event_cb)
      usbp->config->event_cb(usbp, USB_EVENT_CONFIGURED);
    usbSetupTransfer(usbp, NULL, 0);
    return TRUE;
  case USB_RTYPE_RECIPIENT_INTERFACE | (USB_REQ_GET_STATUS << 8):
  case USB_RTYPE_RECIPIENT_ENDPOINT | (USB_REQ_SYNCH_FRAME << 8):
    /* Just sending two zero bytes, the application can change the behavior
       using a hook..*/
    usbSetupTransfer(usbp, (uint8_t *)zero_status, 2);
    return TRUE;
  case USB_RTYPE_RECIPIENT_ENDPOINT | (USB_REQ_GET_STATUS << 8):
    /* Sending the EP status.*/
    if (usbp->setup[4] & 0x80) {
      switch (usb_lld_get_status_in(usbp, usbp->setup[4] & 0x0F)) {
      case EP_STATUS_STALLED:
        usbSetupTransfer(usbp, (uint8_t *)halted_status, 2);
        return TRUE;
      case EP_STATUS_ACTIVE:
        usbSetupTransfer(usbp, (uint8_t *)active_status, 2);
        return TRUE;
      default:
        return FALSE;
      }
    }
    else {
      switch (usb_lld_get_status_out(usbp, usbp->setup[4] & 0x0F)) {
      case EP_STATUS_STALLED:
        usbSetupTransfer(usbp, (uint8_t *)halted_status, 2);
        return TRUE;
      case EP_STATUS_ACTIVE:
        usbSetupTransfer(usbp, (uint8_t *)active_status, 2);
        return TRUE;
      default:
        return FALSE;
      }
    }
  case USB_RTYPE_RECIPIENT_ENDPOINT | (USB_REQ_CLEAR_FEATURE << 8):
    /* Only ENDPOINT_HALT is handled as feature.*/
    if (usbp->setup[2] != USB_FEATURE_ENDPOINT_HALT)
      return FALSE;
    /* Clearing the EP status, not valid for EP0, it is ignored in that case.*/
    if ((usbp->setup[4] & 0x0F) > 0) {
      if (usbp->setup[4] & 0x80)
        usb_lld_clear_in(usbp, usbp->setup[4] & 0x0F);
      else
        usb_lld_clear_out(usbp, usbp->setup[4] & 0x0F);
    }
    usbSetupTransfer(usbp, NULL, 0);
    return TRUE;
  case USB_RTYPE_RECIPIENT_ENDPOINT | (USB_REQ_SET_FEATURE << 8):
    /* Only ENDPOINT_HALT is handled as feature.*/
    if (usbp->setup[2] != USB_FEATURE_ENDPOINT_HALT)
      return FALSE;
    /* Stalling the EP, not valid for EP0, it is ignored in that case.*/
    if ((usbp->setup[4] & 0x0F) > 0) {
      if (usbp->setup[4] & 0x80)
        usb_lld_stall_in(usbp, usbp->setup[4] & 0x0F);
      else
        usb_lld_stall_out(usbp, usbp->setup[4] & 0x0F);
    }
    usbSetupTransfer(usbp, NULL, 0);
    return TRUE;
  case USB_RTYPE_RECIPIENT_DEVICE | (USB_REQ_SET_DESCRIPTOR << 8):
  case USB_RTYPE_RECIPIENT_INTERFACE | (USB_REQ_CLEAR_FEATURE << 8):
  case USB_RTYPE_RECIPIENT_INTERFACE | (USB_REQ_SET_FEATURE << 8):
  case USB_RTYPE_RECIPIENT_INTERFACE | (USB_REQ_GET_INTERFACE << 8):
  case USB_RTYPE_RECIPIENT_INTERFACE | (USB_REQ_SET_INTERFACE << 8):
    /* All the above requests are not handled here, if you need them then
       use the hook mechanism and provide handling.*/
  default:
    return FALSE;
  }
}

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   USB Driver initialization.
 * @note    This function is implicitly invoked by @p halInit(), there is
 *          no need to explicitly initialize the driver.
 *
 * @init
 */
void usbInit(void) {

  usb_lld_init();
}

/**
 * @brief   Initializes the standard part of a @p USBDriver structure.
 *
 * @param[out] usbp     pointer to the @p USBDriver object
 *
 * @init
 */
void usbObjectInit(USBDriver *usbp) {

  usbp->state  = USB_STOP;
  usbp->config = NULL;
  usbp->param  = NULL;
}

/**
 * @brief   Configures and activates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] config    pointer to the @p USBConfig object
 *
 * @api
 */
void usbStart(USBDriver *usbp, const USBConfig *config) {
  unsigned i;

  chDbgCheck((usbp != NULL) && (config != NULL), "usbStart");

  chSysLock();
  chDbgAssert((usbp->state == USB_STOP) || (usbp->state == USB_READY),
              "usbStart(), #1", "invalid state");
  usbp->config = config;
  for (i = 0; i <= USB_MAX_ENDPOINTS; i++)
    usbp->ep[i] = NULL;
  usb_lld_start(usbp);
  usbp->state = USB_READY;
  chSysUnlock();
}

/**
 * @brief   Deactivates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @api
 */
void usbStop(USBDriver *usbp) {

  chDbgCheck(usbp != NULL, "usbStop");

  chSysLock();
  chDbgAssert((usbp->state == USB_STOP) || (usbp->state == USB_READY),
              "usbStop(), #1", "invalid state");
  usb_lld_stop(usbp);
  usbp->state = USB_STOP;
  chSysUnlock();
}

/**
 * @brief   Enables an endpoint.
 * @details This function enables an endpoint, both IN and/or OUT directions
 *          depending on the configuration structure.
 * @note    This function must be invoked in response of a SET_CONFIGURATION
 *          or SET_INTERFACE message.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[out] epp      pointer to an endpoint state descriptor structure
 * @param[in] epcp      the endpoint configuration
 *
 * @iclass
 */
void usbInitEndpointI(USBDriver *usbp, usbep_t ep, USBEndpointState *epp,
                      const USBEndpointConfig *epcp) {

  chDbgAssert(usbp->state == USB_ACTIVE,
              "usbEnableEndpointI(), #1", "invalid state");
  chDbgAssert(usbp->ep[ep] != NULL,
              "usbEnableEndpointI(), #2", "already initialized");

  /* Logically enabling the endpoint in the USBDriver structure.*/
  memset(epp, 0, sizeof(USBEndpointState));
  epp->config  = epcp;
  usbp->ep[ep] = epp;

  /* Low level endpoint activation.*/
  usb_lld_init_endpoint(usbp, ep);
}

/**
 * @brief   Disables all the active endpoints.
 * @details This function disables all the active endpoints except the
 *          endpoint zero.
 * @note    This function must be invoked in response of a SET_CONFIGURATION
 *          message with configuration number zero.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @iclass
 */
void usbDisableEndpointsI(USBDriver *usbp) {
  unsigned i;

  chDbgAssert(usbp->state == USB_SELECTED,
              "usbDisableEndpointsI(), #1", "invalid state");

  for (i = 1; i <= USB_MAX_ENDPOINTS; i++)
    usbp->ep[i] = NULL;

  /* Low level endpoints deactivation.*/
  usb_lld_disable_endpoints(usbp);
}

/**
 * @brief   Reads a packet from the dedicated packet buffer.
 * @pre     In order to use this function he endpoint must have been
 *          initialized in packet mode.
 * @post    The endpoint is ready to accept another packet.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 * @return              The received packet size regardless the specified
 *                      @p n parameter.
 * @retval USB_ENDPOINT_BUSY Endpoint busy receiving.
 * @retval 0            Zero size packet received.
 *
 * @iclass
 */
size_t usbReadPacketI(USBDriver *usbp, usbep_t ep,
                      uint8_t *buf, size_t n) {

  if (usbp->ep[ep]->receiving)
    return USB_ENDPOINT_BUSY;

  return usb_lld_read_packet(usbp, ep, buf, n);;
}

/**
 * @brief   Writes a packet to the dedicated packet buffer.
 * @pre     In order to use this function he endpoint must have been
 *          initialized in packet mode.
 * @post    The endpoint is ready to transmit the packet.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[in] buf       buffer where to fetch the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 * @return              The operation status.
 * @retval USB_ENDPOINT_BUSY Endpoint busy transmitting.
 * @retval 0            Operation complete.
 *
 * @iclass
 */
size_t usbWritePacketI(USBDriver *usbp, usbep_t ep,
                       const uint8_t *buf, size_t n) {

  if (usbp->ep[ep]->transmitting)
    return USB_ENDPOINT_BUSY;

  usb_lld_write_packet(usbp, ep, buf, n);
  return 0;
}

/**
 * @brief   Starts a receive operation on an OUT endpoint.
 * @pre     In order to use this function he endpoint must have been
 *          initialized in transaction mode.
 * @post    The endpoint callback is invoked when the transfer has been
 *          completed.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the received data
 * @param[in] n         maximum number of bytes to copy
 * @return              The operation status.
 * @retval FALSE        Operation complete.
 * @retval TRUE         Endpoint busy receiving.
 *
 * @iclass
 */
bool_t usbStartReceiveI(USBDriver *usbp, usbep_t ep,
                        uint8_t *buf, size_t n) {

  if (usbp->ep[ep]->receiving)
    return TRUE;
  usbp->ep[ep]->receiving = TRUE;
  usb_lld_start_out(usbp, ep, buf, n);
  return FALSE;
}

/**
 * @brief   Starts a transmit operation on an IN endpoint.
 * @pre     In order to use this function he endpoint must have been
 *          initialized in transaction mode.
 * @post    The endpoint callback is invoked when the transfer has been
 *          completed.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[in] buf       buffer where to fetch the data to be transmitted
 * @param[in] n         maximum number of bytes to copy
 * @return              The operation status.
 * @retval FALSE        Operation complete.
 * @retval TRUE         Endpoint busy transmitting.
 *
 * @iclass
 */
bool_t usbStartTransmitI(USBDriver *usbp, usbep_t ep,
                         const uint8_t *buf, size_t n) {

  if (usbp->ep[ep]->transmitting)
    return TRUE;
  usbp->ep[ep]->transmitting = TRUE;
  usb_lld_start_in(usbp, ep, buf, n);
  return FALSE;
}

/**
 * @brief   Stalls an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The operation status.
 * @retval FALSE        Endpoint stalled.
 * @retval TRUE         The endpoint was within a transaction, not stalled.
 *
 * @iclass
 */
bool_t usbStallReceiveI(USBDriver *usbp, usbep_t ep) {

  if (usbp->ep[ep]->receiving)
    return TRUE;
  usb_lld_stall_out(usbp, ep);
  return FALSE;
}

/**
 * @brief   Stalls an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The operation status.
 * @retval FALSE        Endpoint stalled.
 * @retval TRUE         The endpoint was within a transaction, not stalled.
 *
 * @iclass
 */
bool_t usbStallTransmitI(USBDriver *usbp, usbep_t ep) {

  if (usbp->ep[ep]->transmitting)
    return TRUE;
  usb_lld_stall_in(usbp, ep);
  return FALSE;
}

/**
 * @brief   USB reset routine.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void _usb_reset(USBDriver *usbp) {
  unsigned i;

  usbp->state         = USB_READY;
  usbp->status        = 0;
  usbp->address       = 0;
  usbp->configuration = 0;

  /* Invalidates all endpoints into the USBDriver structure.*/
  for (i = 0; i <= USB_MAX_ENDPOINTS; i++)
    usbp->ep[i] = NULL;

  /* EP0 state machine initialization.*/
  usbp->ep0state = USB_EP0_WAITING_SETUP;

  /* Low level reset.*/
  usb_lld_reset(usbp);
}

/**
 * @brief   Default EP0 IN callback.
 * @details This function is used by the low level driver as default handler
 *          for EP0 IN events.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number, always zero
 *
 * @notapi
 */
void _usb_ep0in(USBDriver *usbp, usbep_t ep) {
  size_t max;

  (void)ep;
  switch (usbp->ep0state) {
  case USB_EP0_TX:
    max = usb_lld_fetch_word(&usbp->setup[6]);
     /* If the transmitted size is less than the requested size and it is a
        multiple of the maximum packet size then a zero size packet must be
        transmitted.*/
     if ((usbp->ep0n < max) &&
         ((usbp->ep0n % usbp->ep[0]->config->in_maxsize) == 0)) {
       usb_lld_start_in(usbp, 0, NULL, 0);
       return;
     }
     usbp->ep0state = USB_EP0_WAITING_STS;
     usb_lld_start_out(usbp, 0, NULL, 0);
    return;
  case USB_EP0_SENDING_STS:
#if USB_SET_ADDRESS_MODE == USB_LATE_SET_ADDRESS
    if ((usbp->setup[0] == USB_RTYPE_RECIPIENT_DEVICE) &&
        (usbp->setup[1] == USB_REQ_SET_ADDRESS))
      set_address(usbp);
#endif
    usbp->ep0state = USB_EP0_WAITING_SETUP;
    return;
  default:
    ;
  }
  /* Error response.*/
  usb_lld_stall_in(usbp, 0);
  usb_lld_stall_out(usbp, 0);
  if (usbp->config->event_cb)
    usbp->config->event_cb(usbp, USB_EVENT_STALLED);
  usbp->ep0state = USB_EP0_WAITING_SETUP;
}

/**
 * @brief   Default EP0 OUT callback.
 * @details This function is used by the low level driver as default handler
 *          for EP0 OUT events.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number, always zero
 *
 * @notapi
 */
void _usb_ep0out(USBDriver *usbp, usbep_t ep) {
  size_t n, max;

  (void)ep;
  switch (usbp->ep0state) {
  case USB_EP0_WAITING_SETUP:
    /* SETUP packet handling.
       First verify if the application has an handler installed for this
       request.*/
    if (!(usbp->config->requests_hook_cb) ||
        !(usbp->config->requests_hook_cb(usbp))) {
      /* Invoking the default handler, if this fails then stalls the
         endpoint zero as error.*/
      if (((usbp->setup[0] & USB_RTYPE_TYPE_MASK) != USB_RTYPE_TYPE_STD) ||
          !default_handler(usbp))
        break;
    }

    /* Transfer preparation. The request handler must have populated
       correctly the fields ep0next, ep0n and ep0endcb using
       the macro usbSetupTransfer().*/
    max = usb_lld_fetch_word(&usbp->setup[6]);
    /* The transfer size cannot exceed the specified amount.*/
    if (usbp->ep0n > max)
      usbp->ep0n = max;
    if ((usbp->setup[0] & USB_RTYPE_DIR_MASK) == USB_RTYPE_DIR_DEV2HOST) {
      /* IN phase.*/
      if (usbp->ep0n > 0) {
        /* Starts transmission.*/
        usbp->ep0state = USB_EP0_TX;
        usb_lld_start_in(usbp, 0, usbp->ep0next, usbp->ep0n);
      }
      else {
        /* Receiving the zero sized status packet.*/
        usbp->ep0state = USB_EP0_WAITING_STS;
        usb_lld_start_out(usbp, 0, NULL, 0);
      }
    }
    else {
      /* OUT phase.*/
      if (usbp->ep0n > 0) {
        /* Starts reception.*/
        usbp->ep0state = USB_EP0_RX;
        usb_lld_start_out(usbp, 0, usbp->ep0next, usbp->ep0n);
      }
      else {
        /* Sending zero sized status packet.*/
        usbp->ep0state = USB_EP0_SENDING_STS;
        usb_lld_start_in(usbp, 0, NULL, 0);
      }
    }
    return;
  case USB_EP0_RX:
    usbp->ep0state = USB_EP0_SENDING_STS;
    usb_lld_start_in(usbp, 0, NULL, 0);
    return;
  case USB_EP0_WAITING_STS:
    /* STATUS received packet handling, it must be zero sized.*/
    n = usbp->ep[0]->rxsize;
    if (n != 0)
      break;
    usbp->ep0state = USB_EP0_WAITING_SETUP;
    return;
  default:
    ;
  }
  /* Error response.*/
  usb_lld_stall_in(usbp, 0);
  usb_lld_stall_out(usbp, 0);
  if (usbp->config->event_cb)
    usbp->config->event_cb(usbp, USB_EVENT_STALLED);
  usbp->ep0state = USB_EP0_WAITING_SETUP;
}

#endif /* HAL_USE_USB */

/** @} */